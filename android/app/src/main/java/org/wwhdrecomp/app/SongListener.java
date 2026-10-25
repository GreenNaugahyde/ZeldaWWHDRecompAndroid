package org.wwhdrecomp.app;

import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.os.Handler;
import android.os.Looper;

/**
 * Mods > Sing the songs (experimental, pref sing_songs): the Wind Waker's songs sung, hummed or whistled into the
 * microphone (SongMatcher recognises them) are played by the game as if Link had conducted them
 * (runtime/src/mods/songs.cpp answers the game's judge with the song).
 * On or off: the microphone listens only while Link conducts (the game mutes its music then). (An earlier "Anytime"
 * mode, listening while playing and taking the Wind Waker out by itself, was dropped; its stored value 2 counts as on.)
 * Mic button (pref sing_button, on by default): the microphone only listens while the mic button (ControlsView, shown
 * when singing is possible) or L on a controller is held; letting go judges the song at once. Off: it listens by
 * itself (a pause of 0.7 s ends a song). A touch hint explains it the first time (GameHint 20 / 21).
 * Songs Link hasn't learned are named but not played. Nothing is recorded or kept: the sound is only analysed.
 * Each hold of the mic button while Link conducts is a take (Take: the pitch heard, the game's beats and the note it
 * took at each), which SongPanel shows as a timeline under the game's own note row. Practice (offered after the
 * first hint, GameHint 22): takes as usual, but the game gets no notes and no song, the panel says what was heard.
 */
final class SongListener implements SongMatcher.Listener {
    static final int OFF = 0, ON = 1;
    static final int PERMISSION_REQUEST = 8;

    private final MainActivity a;
    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile boolean running;
    private volatile int generation;  // each microphone thread's; a newer one ends the older
    private Thread thread;
    private boolean resumed;
    private int[] state = new int[9];
    static final int HINT_BUTTON = 20, HINT_AUTO = 21, HINT_TRY = 22, HINT_PLAY = 23;
    private boolean buttonShown, touchHeld, padHeld;
    private volatile int finishGen = -1;  // the microphone thread that ends its phrase when it stops (button let go)

    SongListener(MainActivity a) { this.a = a; }

    int mode() { return a.prefs.getInt("sing_songs", OFF) != OFF ? ON : OFF; }

    boolean conducting() { return state[0] != 0; }

    boolean useButton() { return a.prefs.getBoolean("sing_button", true); }

    /** the mic button is on screen now (ControlsView draws it; L on a controller holds it too) */
    boolean buttonShown() { return buttonShown; }

    /** the mic button held by touch / by the controller's L */
    void setHeld(boolean touch, boolean on) {
        if (touch) touchHeld = on; else padHeld = on;
        main.removeCallbacks(poll);
        poll.run();  // at once: letting go judges the song now
    }

    /** conducting by voice: while the mic button is held and Link conducts, the game gets the C stick in the
     *  direction of the note sung last (cStick(): the game shows its note, plays it, and on each beat takes it, so a
     *  song sung with the metronome is the game's own; MainActivity.pushInput), in the time of the switch
     *  (timeBySwitch). Letting go centres the C stick; a song recognised then still plays (songs.cpp, unless the
     *  game took it itself) */
    boolean conductsByVoice() { return timeBySwitch() && (touchHeld || padHeld); }

    /** the time switch's time goes to the game whenever Link conducts with the mic button shown (a tap shows at once
     *  in the game's own time display and pose; the left stick doesn't pick the time then) */
    boolean timeBySwitch() { return buttonShown && conducting(); }

    private int pushed;  // what the sticks last got: bit 0 the time, bit 1 the notes

    /** the time the mic button's switch shows: 3, 4 or 6 (pref sing_beat) */
    int beat() {
        int b = a.prefs.getInt("sing_beat", 3);
        return b == 4 || b == 6 ? b : 3;
    }

    /** the time switch tapped: 3/4 -> 4/4 -> 6/4 -> 3/4 */
    void nextBeat() {
        int b = beat();
        a.prefs.edit().putInt("sing_beat", b == 3 ? 4 : b == 4 ? 6 : 3).apply();
        liveDir = 0;
        if (liveBeat != 0) {  // switched while singing: the notes from here on in the new time
            liveBeat = beat();
            Take k = take;
            if (k != null && k.end < 0) {
                k.beat = liveBeat;
                k.beatMs = beatMs();
                k.key = Float.NaN;
                k.ref = Float.NaN;  // (the middle note was hummed in the other time's count)
            }
        }
        a.repushInput();
    }

    /** the C stick's direction for the game while conducting by voice: 0 centre, 1 up, 2 right, 3 down, 4 left
     *  (practice: always centre, the game takes no notes) */
    int cStick() { return practice ? 0 : nudging() ? 1 : liveDir; }

    // ---- a song the app recognised but the game's judge didn't take (sung off its beats) goes to the game through
    // the judge's next answer (songs.cpp play_song). The judge only runs on the game's beats, and its metronome only
    // while the C stick is off the centre: once the take is over (its last note taken: the game's own judge had its
    // chance) the stick is pushed up until the judge has taken the song (songState[4] counts the songs handed) or
    // NUDGE_MS have passed.
    private static final int NUDGE_MS = 3000;
    private volatile long nudgeUntil;
    private int nudgeHanded;
    private boolean nudgeWaiting;  // a song handed, the take not over yet

    /** the C stick is pushed for a recognised song (MainActivity sends it even with the mic button let go) */
    boolean nudging() { return nudgeUntil != 0; }

    // ---- a play button on the game's song list tapped (ControlsView; or the one a story scene waiting for a song
    // shows, learned or not): the song goes to the judge's next answer as a
    // sung one does, the C stick pushed up so the judge runs, the left stick in the song's time
    private static final int[] TIMES = {3, 4, 4, 6, 6, 3};  // by melody: Requiem, Ballad, Command, Earth, Wind, Passing
    private volatile int listTime;  // the song's time while it is pushed, 0 none

    /** the time to hold the left stick in while a song from the list is pushed (3, 4, 6), 0 none */
    int listTime() { return nudging() ? listTime : 0; }

    /** play buttons on the game's song list (pref song_list_play, Mods; on by default) */
    boolean listPlay() { return a.prefs.getBoolean("song_list_play", true); }

    /** a play button for the song a story scene waits for (pref song_scene_play, Mods; on by default) */
    boolean scenePlay() { return a.prefs.getBoolean("song_scene_play", true); }

    void playFromList(int song) {
        int[] s = a.gameRunning() ? Native.songState() : new int[9];
        if (s[0] == 0 || s[1] >= 0 && (s[1] & (1 << song)) == 0 && s[8] != song) return;  // (not conducting / not learned, not the scene's)
        Native.playSong(song);
        nudgeHanded = s[4];
        nudgeWaiting = false;
        listTime = TIMES[song];
        nudgeUntil = android.os.SystemClock.uptimeMillis() + NUDGE_MS;
        a.repushInput();
    }

    private volatile int liveDir;
    private volatile int liveBeat, liveLearned;

    /** the count-in now (ControlsView shows it beside the game's time bar): 1.. the time's beats (each bar), 0 none */
    int countNow() {
        int c = conductsByVoice() ? count() : 0;
        return c == 0 ? 0 : (c - 1) % liveBeatOr() + 1;
    }

    // ---- takes: what SongPanel draws. One per hold of the mic button while Link conducts; the last one stays
    // (shown a moment after letting go, in practice until the next)
    static final class Take {
        static final int SAMPLES = 1024, BEATS = 64;
        long start, end = -1;     // the press (the count-in's first beat), letting go (uptime ms)
        int beat, beatMs;         // the time (3, 4, 6) and a beat's length
        boolean practice;
        // the pitch heard, per microphone frame (20 ms): its time, MIDI (NaN: none); a ring, n counts all
        final long[] pt = new long[SAMPLES];
        final float[] pp = new float[SAMPLES];
        volatile int pn;
        // the beats the game took (practice: the app's clock): time, direction (C U R D L = 0..4; -1 none)
        final long[] bt = new long[BEATS];
        final int[] bd = new int[BEATS];
        volatile int bn;
        volatile float key = Float.NaN;  // the singer's key: shift onto SongMatcher.scale(beat, alt)
        volatile boolean alt;
        // the singer's middle note (MIDI): hummed in the count-in's first bar (the median of what was heard), NaN none
        volatile float ref = Float.NaN;
        final float[] refs = new float[REF_SAMPLES];
        int rn;
        static final int REF_SAMPLES = 256, REF_MIN = 8;  // (20 ms frames: at least 160 ms of humming)

        /** the count-in: COUNT_BARS bars from start */
        long countEnd() { return start + (long) COUNT_BARS * beat * beatMs; }
        volatile int dir;                // the note sent now
        volatile String result;          // after letting go: what was heard (practice), null none yet
        volatile boolean found;          // ... a song
        volatile boolean firstPending, firstSent;  // the first note: waiting (FIRST_LAG_MS), sent (beats begin)
        volatile boolean done;           // a song's worth of notes sung (the time's beats): the rest is ignored
        volatile int doneBeats;          // ... beats taken by then (the last note stays until the next is taken)
        volatile long doneAt;

        /** something was heard since then */
        boolean heard(long since) {
            int n = pn;
            for (int i = n - 1; i >= Math.max(0, n - SAMPLES); i--) {
                if (pt[i % SAMPLES] <= since) break;
                if (!Float.isNaN(pp[i % SAMPLES])) return true;
            }
            return false;
        }

        void beat(long t, int d) {
            int i = bn;
            if (i >= BEATS) return;
            bt[i] = t;
            bd[i] = d;
            bn = i + 1;
        }
    }

    private volatile Take take;
    private long micEpoch;  // uptime of the microphone's first sample (frame times count from it)
    private int frames;

    /** the take to show now, or null */
    Take take() {
        Take t = take;
        if (t == null || !buttonShown || !conducting()) return null;
        if (t.end >= 0 && !practice && android.os.SystemClock.uptimeMillis() - t.end > 2500) return null;
        return t;
    }

    // practice: on from the hint's question until Link stops conducting (or the panel's close mark)
    private boolean practice;

    boolean practicing() { return practice && buttonShown && conducting(); }

    void startPractice() {
        practice = true;
        take = null;
        a.repushInput();
    }

    void stopPractice() {
        practice = false;
        take = null;
        a.repushInput();
    }

    @Override
    public void onPitch(float midi) {  // (the microphone's thread)
        long t = micEpoch + (long) (frames++ * SongMatcher.HOP + SongMatcher.HOP * 1.6f) * 1000 / SongMatcher.RATE;
        Take k = take;
        if (k == null || k.end >= 0) return;
        if (!Float.isNaN(midi) && t >= k.start && t < k.start + (long) k.beat * k.beatMs && k.rn < Take.REF_SAMPLES) {
            k.refs[k.rn++] = midi;  // the count-in's first bar: the middle note
            if (k.rn >= Take.REF_MIN) {
                float[] v = java.util.Arrays.copyOf(k.refs, k.rn);
                java.util.Arrays.sort(v);
                k.ref = v[v.length / 2];
            }
        }
        int i = k.pn;
        k.pt[i % Take.SAMPLES] = t;
        k.pp[i % Take.SAMPLES] = midi;
        k.pn = i + 1;
    }

    // ---- the count-in: holding the mic button while Link conducts counts two bars (3, 4 or 6 beats each) at the
    // game's tempo for that time (JAIZelInst::setBeat: 30, 25.7, 22.5 frames a beat), shown as numbers; the beat
    // after it is the song's first note. In the first bar the singer hums their middle note (Take.ref: the songs'
    // notes are placed around it, so a group's first note is known by its pitch), the second is a breath. The game's own metronome (HD) starts only when the C stick leaves the centre, so it
    // starts with that note and the following ones, in the count's tempo, land on its beats. Notes sung before
    // don't count (the matcher starts afresh), the C stick stays centred until then. Each count sounds the game's own
    // metronome click (the last also its upbeat; songs.cpp count_beat).
    static final int COUNT_BARS = 2;
    private volatile long countStart = -1;  // uptime of the press, -1 none
    private volatile boolean countedIn;

    private int beatMs() {
        int b = liveBeatOr();
        return b == 4 ? 857 : b == 6 ? 750 : 1000;
    }

    private int liveBeatOr() { return liveBeat != 0 ? liveBeat : beat(); }

    /** the count-in now: 1.. the time's beats, 0 none (over, or not counting) */
    private int count() {
        long from = countStart;
        if (from < 0) return 0;
        long k = 1 + (android.os.SystemClock.uptimeMillis() - from) / beatMs();
        return k <= (long) COUNT_BARS * liveBeatOr() ? (int) k : 0;
    }

    /** checked by the microphone's thread: the count-in just ended (half a beat before the first note: the matcher
     *  starts afresh) */
    private boolean countInEnded() {
        long from = countStart;
        if (countedIn || from < 0) return false;
        if (android.os.SystemClock.uptimeMillis() - from < (long) COUNT_BARS * liveBeatOr() * beatMs() - beatMs() / 2) return false;
        countedIn = true;
        return true;
    }

    @Override
    public void onNote(float[] p) {  // (the microphone's thread)
        Take k = take;
        if (liveBeat == 0 || !countedIn || k != null && k.done) return;
        int n = SongMatcher.noteCount(p, liveBeat);
        if (k != null && n > k.beat) return;  // (past the song's last note: ignored)
        float[] key = new float[2];
        int d = SongMatcher.direction(p, liveBeat, liveLearned, key, k != null ? k.ref : Float.NaN);
        if (k != null) {
            k.key = key[0];
            k.alt = key[1] != 0;
            k.dir = d;
        }
        android.util.Log.i("wwhd", "songs: note " + p.length + " (" + String.format(java.util.Locale.ROOT, "%.1f", p[p.length - 1]) + "): direction " + d);
        if (k != null && n == k.beat) {  // the song's last note: the take is done (the microphone stops, the poll)
            k.doneBeats = k.bn;
            k.doneAt = android.os.SystemClock.uptimeMillis();
            k.done = true;
        }
        if (k != null && !k.firstSent) {
            // the take's first note: HD's metronome starts when the C stick leaves the centre, its beats then come
            // a beat apart. A note is known MIN_NOTE frames after it starts, so notes sung in tempo would be known
            // just at the game's beat (a toss-up whether it takes the new note or the one before): the first note
            // goes out FIRST_LAG_MS later, the game's beats (practice: ours) that much after each note is known
            if (k.firstPending) return;  // (sent with the newest direction)
            k.firstPending = true;
            main.postDelayed(() -> {
                if (take != k || k.end >= 0) return;
                k.firstSent = true;
                liveDir = k.dir;
                a.repushInput();
                if (k.practice) practiceBeat(k, android.os.SystemClock.uptimeMillis());
            }, FIRST_LAG_MS);
            return;
        }
        if (d == liveDir) return;
        liveDir = d;
        main.post(a::repushInput);
    }

    private static final int FIRST_LAG_MS = 150;

    boolean permitted() { return a.checkSelfPermission(android.Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED; }

    // debug: WWHD_SONG_TEST=<a sound file> plays it on the device's speaker and listens to it (the microphone path
    // as on a device; results in the log as "songs: heard ..."), once per start
    private boolean testing, tested;
    private android.media.MediaPlayer testPlayer; // held here: a player only a local refers to is collected mid-play

    private void selfTest(String path) {
        tested = true;
        if (!permitted()) {
            android.util.Log.w("wwhd", "songs: test without the microphone permission");
            return;
        }
        testing = true;
        startMic();
        android.media.MediaPlayer p = testPlayer = new android.media.MediaPlayer();
        try {
            p.setDataSource(path);
            p.setOnCompletionListener(x -> main.postDelayed(() -> { testing = false; x.release(); testPlayer = null; android.util.Log.i("wwhd", "songs: test done"); }, 2000));
            p.prepare();
            p.start();
            android.util.Log.i("wwhd", "songs: test playing " + path);
        } catch (java.io.IOException | RuntimeException e) {
            android.util.Log.w("wwhd", "songs: test: " + e);
            testing = false;
            p.release();
            testPlayer = null;
        }
    }

    void resume() {
        resumed = true;
        String test = android.system.Os.getenv("WWHD_SONG_TEST");
        if (test != null && !tested) main.postDelayed(() -> selfTest(test), 1000);
        main.removeCallbacks(poll);
        main.post(poll);
    }

    void pause() {
        resumed = false;
        main.removeCallbacks(poll);
        stopMic();
        touchHeld = padHeld = buttonShown = false;
        show(false, 0);
    }

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (!resumed) return;
            int mode = mode();
            int beats = state[5];
            state = mode == OFF && !listPlay() && !scenePlay() || !a.gameRunning() ? new int[9] : Native.songState();
            boolean conducting = state[0] != 0;
            a.showSongList(conducting && listPlay() ? (state[1] >= 0 ? state[1] & 0x3F : 0x3F) : 0);
            a.showSongWanted(conducting && scenePlay() ? state[8] : -1);  // a story scene waits for this song
            boolean possible = permitted() && mode == ON && conducting;
            boolean button = useButton();
            buttonShown = possible && button;
            if (!buttonShown) touchHeld = padHeld = false;
            if (!conducting) practice = false;
            Take k = take;
            if (k != null && k.end < 0 && !practice && state[5] != beats && state[5] != 0) {  // the game took a beat
                long t = android.os.SystemClock.uptimeMillis() - state[7];
                if (t >= k.start) k.beat(t, state[6] & 15);
            }
            // a song's worth of notes sung (3, 4 or 6): the microphone stops (the phrase goes to the matcher, notes sung
            // while the button is still held count no more: letting go just in time is hard); the last note stays on
            // the C stick until the game (practice: the app) has taken the next beat, then the take ends as if let go
            if (k != null && k.end < 0 && k.done
                    && (k.bn > k.doneBeats && k.bt[k.bn - 1] > k.doneAt + 120  // (a beat right then took the note before)
                        || android.os.SystemClock.uptimeMillis() - k.doneAt > 2L * k.beatMs)) {
                k.end = android.os.SystemClock.uptimeMillis();
                liveDir = 0;
                a.repushInput();
            }
            if (!conductsByVoice()) {
                if (countStart >= 0 && k != null && k.end < 0) k.end = android.os.SystemClock.uptimeMillis();
                countStart = -1;
                countedIn = false;
            } else if (countStart < 0) {  // held while Link conducts: the count-in starts
                long start = countStart = android.os.SystemClock.uptimeMillis();
                countedIn = false;
                int time = liveBeatOr(), ms = beatMs();
                Take n = new Take();
                n.start = start;
                n.beat = time;
                n.beatMs = ms;
                n.practice = practice;
                take = n;
                for (int i = 0; i < COUNT_BARS * time; i++) {  // its beats heard: the game's click, the last with its upbeat
                    boolean last = i == COUNT_BARS * time - 1;
                    main.postAtTime(() -> { if (countStart == start) Native.countBeat(time, last); }, start + (long) i * ms);
                }
            }
            if (nudgeWaiting && (k == null || k.end >= 0)) {  // the take is over: push (if still needed)
                nudgeWaiting = false;
                if (state[4] == nudgeHanded && conducting) {
                    nudgeUntil = android.os.SystemClock.uptimeMillis() + NUDGE_MS;
                    a.repushInput();
                }
            }
            if (nudgeUntil != 0 && (state[4] != nudgeHanded || !conducting || android.os.SystemClock.uptimeMillis() > nudgeUntil)) {
                nudgeUntil = 0;  // the judge took the song (or Link stopped conducting, or it never came)
                listTime = 0;
                a.repushInput();
            }
            boolean done = countStart >= 0 && k != null && k.done;  // (until the button is let go)
            boolean listen = testing || possible && (!button || (touchHeld || padHeld) && !done);
            if (possible) a.offerSongHint(button ? HINT_BUTTON : HINT_AUTO);
            if (listen && !running) startMic();
            else if (!listen && running) stopMic(button);
            if (!conductsByVoice()) liveDir = 0;
            int now = (timeBySwitch() ? 1 : 0) | (conductsByVoice() ? 2 : 0);
            if (now != pushed) {  // (a held finger sends no new touches)
                pushed = now;
                a.repushInput();
            }
            show(running, notes);
            main.postDelayed(this, 100);
        }
    };

    /** practice: the game's metronome isn't running (the C stick stays centred), so the beats come from the app's
     *  clock, each with the game's click; as the game's, they start with the first note sung after the count-in
     *  (HD's metronome starts when the C stick leaves the centre), and each takes the note sent then */
    private void practiceBeat(Take k, long at) {
        main.postAtTime(() -> {
            if (take != k || k.end >= 0 || !practice) return;
            Native.countBeat(k.beat, false);
            k.beat(at, k.heard(at - k.beatMs) ? liveDir : -1);  // (nothing sung that beat: no note)
            practiceBeat(k, at + k.beatMs);
        }, at);
    }

    // ---- the microphone (its own thread): 16 kHz mono into the matcher
    private void startMic() {
        running = true;
        notes = 0;
        final int gen = ++generation;
        thread = new Thread(() -> {
            int min = AudioRecord.getMinBufferSize(SongMatcher.RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT);
            AudioRecord rec;
            try {
                rec = new AudioRecord(source(), SongMatcher.RATE, AudioFormat.CHANNEL_IN_MONO,
                        AudioFormat.ENCODING_PCM_16BIT, Math.max(min, SongMatcher.RATE / 2));
            } catch (RuntimeException e) {  // (no permission after all)
                android.util.Log.w("wwhd", "songs: microphone: " + e);
                if (gen == generation) running = false;
                return;
            }
            if (rec.getState() != AudioRecord.STATE_INITIALIZED) {
                android.util.Log.w("wwhd", "songs: microphone not available");
                rec.release();
                if (gen == generation) running = false;
                return;
            }
            boolean button = useButton() && !testing;
            liveDir = 0;
            frames = 0;
            micEpoch = -1;
            liveBeat = button ? beat() : 0;
            liveLearned = Math.max(0, state[1]);
            SongMatcher m = new SongMatcher(this, !button, liveBeat);
            short[] s = new short[SongMatcher.HOP * 4];
            java.io.InputStream feed = feedStream();
            float[] f = new float[s.length];
            java.io.OutputStream dump = dumpStream();
            byte[] b = dump != null ? new byte[s.length * 2] : null;
            rec.startRecording();
            android.util.Log.i("wwhd", "songs: listening");
            while (running && gen == generation) {
                int n = feed != null ? feed(feed, s) : rec.read(s, 0, s.length);
                if (n <= 0) break;
                // frame times: the first read's end is now, its samples just before
                if (micEpoch < 0) micEpoch = android.os.SystemClock.uptimeMillis() - (long) n * 1000 / SongMatcher.RATE;
                if (button && countInEnded()) m.reset();  // the song's first note is next
                for (int i = 0; i < n; i++) f[i] = s[i] / 32768f;
                if (dump != null) {
                    for (int i = 0; i < n; i++) { b[2 * i] = (byte) s[i]; b[2 * i + 1] = (byte) (s[i] >> 8); }
                    try { dump.write(b, 0, 2 * n); } catch (java.io.IOException e) { dump = null; }
                }
                m.feed(f, n);
                if (testing) {  // the level, once a second
                    for (int i = 0; i < n; i++) peak = Math.max(peak, Math.abs(f[i]));
                    if ((counted += n) >= SongMatcher.RATE) {
                        android.util.Log.i("wwhd", String.format(java.util.Locale.ROOT, "songs: test level %.4f, last pitch %.1f", peak, m.lastPitch()));
                        peak = 0;
                        counted = 0;
                    }
                }
            }
            rec.stop();
            rec.release();
            if (feed != null) try { feed.close(); } catch (java.io.IOException ignored) {}
            if (dump != null) try { dump.close(); } catch (java.io.IOException ignored) {}
            if (finishGen == gen) m.finish();  // the button let go: the song as sung so far
            android.util.Log.i("wwhd", "songs: stopped listening");
        }, "songs-mic");
        thread.start();
    }

    private float peak;
    private int counted;

    // debug: WWHD_SONG_SOURCE=<MediaRecorder.AudioSource number> picks the microphone path; WWHD_SONG_REC=<file>
    // appends what the microphone hears as raw 16 kHz mono 16-bit PCM. VOICE_RECOGNITION by default: it takes the
    // device's own sound out (Snapdragon 855 / Adreno 640: the self-test's songs all but vanish), so the game's
    // pulses and chimes don't count as notes; the self-test listens on MIC for that reason
    private int source() {
        try {
            String v = android.system.Os.getenv("WWHD_SONG_SOURCE");
            if (v != null) return Integer.parseInt(v.trim());
        } catch (NumberFormatException ignored) {}
        return testing ? MediaRecorder.AudioSource.MIC : MediaRecorder.AudioSource.VOICE_RECOGNITION;
    }

    // debug: WWHD_SONG_FEED=<file> (raw 16 kHz mono 16-bit PCM, e.g. a WWHD_SONG_REC recording): each hold of the mic
    // button hears that file instead of the microphone, in real time from its start (a take without a voice)
    private static java.io.InputStream feedStream() {
        String path = android.system.Os.getenv("WWHD_SONG_FEED");
        if (path == null) return null;
        try {
            return new java.io.BufferedInputStream(new java.io.FileInputStream(path));
        } catch (java.io.IOException e) {
            android.util.Log.w("wwhd", "songs: feed: " + e);
            return null;
        }
    }

    private long feedNext;

    /** reads the feed file's next block in real time (silence after its end) */
    private int feed(java.io.InputStream in, short[] s) {
        long now = android.os.SystemClock.uptimeMillis();
        if (micEpoch < 0) feedNext = now;  // (a new hold)
        long wait = feedNext - now;
        if (wait > 0) android.os.SystemClock.sleep(wait);
        feedNext += (long) s.length * 1000 / SongMatcher.RATE;
        byte[] b = new byte[s.length * 2];
        int got = 0;
        try {
            while (got < b.length) {
                int r = in.read(b, got, b.length - got);
                if (r < 0) break;
                got += r;
            }
        } catch (java.io.IOException ignored) {}
        java.util.Arrays.fill(s, (short) 0);
        for (int i = 0; i + 1 < got; i += 2) s[i / 2] = (short) ((b[i] & 0xFF) | (b[i + 1] << 8));
        return s.length;
    }

    private static java.io.OutputStream dumpStream() {
        String path = android.system.Os.getenv("WWHD_SONG_REC");
        if (path == null) return null;
        try {
            return new java.io.BufferedOutputStream(new java.io.FileOutputStream(path, true));
        } catch (java.io.IOException e) {
            android.util.Log.w("wwhd", "songs: recording: " + e);
            return null;
        }
    }

    private void stopMic() { stopMic(false); }

    private void stopMic(boolean finish) {
        running = false;
        if (finish) finishGen = generation;
        generation++;
        thread = null;
        notes = 0;
    }

    // ---- results (on the microphone's thread)
    private volatile int notes;

    @Override
    public void onNotes(int count) { notes = count; }

    @Override
    public void onPhrase(int heard, float[] sung) {
        Take k = take;
        float[] p = sung;
        int song = heard;
        if (k != null && k.done && p.length > k.beat) {  // a note begun after the song's last one: not part of it
            p = java.util.Arrays.copyOf(p, k.beat);
            song = SongMatcher.match(p, k.beat);
        }
        StringBuilder b = new StringBuilder();
        for (float v : p) b.append(String.format(java.util.Locale.ROOT, " %.1f", v - p[0]));
        android.util.Log.i("wwhd", "songs: heard " + p.length + " notes (" + b.toString().trim() + "): " + (song >= 0 ? "song " + song : "no song"));
        // sung in another time than the switch's: the song it would be there (a hint which time to pick)
        int other = song < 0 && k != null && p.length >= 3 ? SongMatcher.match(p) : -1;
        if (other >= 0 && SongMatcher.beatOf(other) == k.beat) other = -1;
        final int elsewhere = other;
        final int found = song;
        if (k != null && k.practice) {  // practice: only said, not played
            main.post(() -> practiced(k, found, elsewhere));
            return;
        }
        if (elsewhere >= 0) main.post(() -> a.showSongBanner(wrongTime(elsewhere)));
        if (found < 0) return;
        main.post(() -> sung(found));
    }

    private String wrongTime(int song) {
        return a.getString(R.string.song_wrong_time, GameText.songName(song), SongMatcher.beatOf(song) + "/4");
    }

    private void practiced(Take k, int song, int elsewhere) {
        android.util.Log.i("wwhd", "songs: practice: " + (song >= 0 ? "song " + song : "no song") + (elsewhere >= 0 ? ", in another time song " + elsewhere : ""));
        int learned = state[1];
        if (song < 0 && elsewhere >= 0) k.result = wrongTime(elsewhere);
        else if (song < 0) k.result = a.getString(R.string.song_practice_none);
        else if (learned >= 0 && (learned & (1 << song)) == 0) k.result = a.getString(R.string.song_not_learned, GameText.songName(song));
        else {
            k.result = GameText.songName(song);
            // a learned song: play it for real now? (practice ends; sung once more, Link plays it)
            String ask = a.getString(R.string.song_play_now, k.result);
            main.postDelayed(() -> { if (take == k && practicing()) a.askSong(HINT_PLAY, ask); }, 700);
        }
        k.found = song >= 0;
    }

    private void sung(int song) {
        if (!resumed || mode() == OFF) return;
        String name = GameText.songName(song);
        int learned = state[1];
        if (learned >= 0 && (learned & (1 << song)) == 0) {
            a.showSongBanner(a.getString(R.string.song_not_learned, name));
            return;
        }
        a.showSongBanner("♪ " + name);
        if (state[0] != 0) {
            Native.playSong(song);
            if (timeBySwitch() && !practice) {  // conducting by voice: the game's judge must run once more
                nudgeHanded = state[4];
                nudgeWaiting = true;
            }
        }
    }

    private boolean shownOn, shownButton, shownHeld, shownConducting;
    private int shownNotes = -1;

    private void show(boolean on, int n) {
        boolean held = touchHeld || padHeld;
        if (on == shownOn && n == shownNotes && buttonShown == shownButton && held == shownHeld && conducting() == shownConducting) return;
        shownConducting = conducting();
        shownOn = on;
        shownNotes = n;
        shownButton = buttonShown;
        shownHeld = held;
        a.showSongListening(on, n, buttonShown, held, buttonShown && conducting());
    }
}
