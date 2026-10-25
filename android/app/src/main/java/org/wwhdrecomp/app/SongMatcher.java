package org.wwhdrecomp.app;

/**
 * Recognises the Wind Waker's songs in sung, hummed or whistled sound (plain Java: SongListener feeds it the
 * microphone). Each frame's pitch (YIN), then notes (a steady pitch for at least 100 ms, split at a jump, a
 * pause or a dip in loudness), then a phrase (the notes up to a pause of 0.7 s), matched against the songs by
 * their intervals: any key, any octave, any tempo.
 *   The songs' notes are the game's (the GameCube decompilation's JAIZelInst, the same tables in HD): each song
 *   is a pattern of baton directions in its beat (mMelodyPattern), each direction a pitch per beat
 *   (mCPosToNote3 / 4 / 61 / 62; 6/4 starting up uses the second table).
 */
final class SongMatcher {
    static final int RATE = 16000;
    static final int HOP = 320;  // 20 ms per frame
    private static final int WINDOW = 1024, HALF = 512;
    private static final float FMIN = 70, FMAX = 2500;

    // pitches (MIDI) by direction: centre, up, right, down, left
    private static final int[] BEAT3 = {48, 60, 57, 62, 53}, BEAT4 = {59, 60, 57, 62, 53},
            BEAT61 = {62, 69, 66, 57, 64}, BEAT62 = {62, 69, 67, 57, 64};
    private static final int C = 0, U = 1, R = 2, D = 3, L = 4;
    // melody number (the game's), beat, directions
    private static final int[][] SONGS = {
            {3, U, L, R},           // 0 Wind's Requiem
            {4, D, R, L, U},        // 1 Ballad of Gales
            {4, L, C, R, C},        // 2 Command Melody
            {6, D, D, C, R, L, C},  // 3 Earth God's Lyric
            {6, U, U, D, R, L, R},  // 4 Wind God's Aria
            {3, R, L, D},           // 5 Song of Passing
    };
    static final int SONG_COUNT = SONGS.length;

    // the forms a song may be sung in: its notes; repeated notes run together; the Aria's octave drop sung level
    private static final java.util.List<int[]> forms = new java.util.ArrayList<>();
    private static final java.util.List<Integer> formSong = new java.util.ArrayList<>();
    static {
        for (int s = 0; s < SONGS.length; s++) {
            int[] p = SONGS[s];
            int[] tbl = p[0] == 3 ? BEAT3 : p[0] == 4 ? BEAT4 : p[1] == U ? BEAT62 : BEAT61;
            int[] notes = new int[p.length - 1];
            for (int i = 1; i < p.length; i++) notes[i - 1] = tbl[p[i]];
            addForms(s, notes);
            if (s == 4) {  // the Aria's low A an octave up
                int[] level = notes.clone();
                level[2] += 12;
                addForms(s, level);
            }
        }
    }

    private static void addForms(int song, int[] notes) {
        add(song, notes);
        int n = 0;
        int[] merged = new int[notes.length];
        for (int i = 0; i < notes.length; i++)
            if (i == 0 || notes[i] != notes[i - 1]) merged[n++] = notes[i];
        if (n < notes.length) add(song, java.util.Arrays.copyOf(merged, n));
    }

    private static void add(int song, int[] notes) {
        for (int i = 0; i < forms.size(); i++)
            if (java.util.Arrays.equals(forms.get(i), notes)) return;
        forms.add(notes);
        formSong.add(song);
    }

    interface Listener {
        /** a phrase ended: the song it was (-1 none) and its notes (MIDI, fractional) */
        void onPhrase(int song, float[] notes);
        /** the notes of the phrase so far changed (for a display) */
        default void onNotes(int count) {}
        /** a new note holds (MIN_NOTE frames): the phrase's notes with it last (MIDI, its pitch so far) */
        default void onNote(float[] notes) {}
        /** each frame (20 ms): the pitch heard (MIDI), NaN none */
        default void onPitch(float midi) {}
    }

    private final Listener listener;
    private final float[] buf = new float[WINDOW];
    private int filled;
    private final float[] diff = new float[HALF];

    /** false: only finish() ends a phrase (the mic button: pauses while it is held don't) */
    private final boolean pauseEnds;
    /** the time chosen (3, 4, 6: only its songs count), 0 any */
    private final int beat;

    SongMatcher(Listener l) { this(l, true, 0); }

    SongMatcher(Listener l, boolean pauseEnds, int beat) {
        listener = l;
        this.pauseEnds = pauseEnds;
        this.beat = beat;
    }

    /** samples in -1..1 at RATE */
    void feed(float[] s, int n) {
        for (int i = 0; i < n; i++) {
            buf[filled++] = s[i];
            if (filled == WINDOW) {
                frame();
                System.arraycopy(buf, HOP, buf, 0, WINDOW - HOP);
                filled = WINDOW - HOP;
            }
        }
    }

    /** the singer is done (the mic button let go): the phrase ends now, without waiting for the pause */
    void finish() {
        jump.clear();
        endPhrase();
    }

    void reset() {
        filled = 0;
        note.clear();
        phrase.clear();
        quiet = gap = 0;
        jump.clear();
        noteHeld = false;
        listener.onNotes(0);
    }

    // ---- per frame: loudness and pitch
    private float noiseFloor = 0.002f, last = Float.NaN;

    /** the last pitch found (MIDI; tests) */
    float lastPitch() { return last; }

    private void frame() {
        double e = 0;
        for (int i = 0; i < WINDOW; i++) e += buf[i] * buf[i];
        float rms = (float) Math.sqrt(e / WINDOW);
        // the noise floor follows quiet frames quickly and loud ones slowly, but not sung ones: a long phrase
        // without pauses (or a room's echo filling them) would raise it over the voice within seconds
        // (a low minimum: VOICE_RECOGNITION delivers a hum at rms 0.005 on Snapdragon 855 / Adreno 640)
        float gate = Math.max(0.0015f, noiseFloor * 4);
        float midi = rms > gate ? pitch() : Float.NaN;
        if (rms < noiseFloor) noiseFloor = noiseFloor * 0.9f + rms * 0.1f;
        else if (Float.isNaN(midi)) noiseFloor = noiseFloor * 0.999f + rms * 0.001f;
        if (!Float.isNaN(midi)) last = midi;
        listener.onPitch(midi);
        step(midi, rms);
    }

    /** YIN: the period with the first dip of the cumulative mean normalised difference below 0.15; MIDI or NaN */
    private float pitch() {
        int tmin = (int) (RATE / FMAX), tmax = Math.min(HALF - 1, (int) (RATE / FMIN));
        float sum = 0;
        diff[0] = 1;
        for (int t = 1; t <= tmax; t++) {
            float d = 0;
            for (int j = 0; j < HALF; j++) {
                float x = buf[j] - buf[j + t];
                d += x * x;
            }
            sum += d;
            diff[t] = sum > 0 ? d * t / sum : 1;
        }
        int best = -1;
        for (int t = Math.max(2, tmin); t < tmax; t++)
            if (diff[t] < 0.15f) {
                while (t + 1 < tmax && diff[t + 1] < diff[t]) t++;
                best = t;
                break;
            }
        if (best < 0) return Float.NaN;
        float a = diff[best - 1], b = diff[best], c = diff[best + 1];
        float den = a - 2 * b + c, shift = den != 0 ? 0.5f * (a - c) / den : 0;
        float period = best + Math.max(-1, Math.min(1, shift));
        float f = RATE / period;
        return (float) (69 + 12 * Math.log(f / 440.0) / Math.log(2));
    }

    // ---- notes
    private static final int MIN_NOTE = 5, GAP = 2, JUMP = 3, PHRASE_END = 35, MAX_NOTES = 10;
    private final java.util.ArrayList<Float> note = new java.util.ArrayList<>(), jump = new java.util.ArrayList<>();
    private float notePeak;
    private int quiet, gap;
    private boolean noteHeld;  // onNote told for the note now
    private final java.util.ArrayList<Float> phrase = new java.util.ArrayList<>();

    private void step(float midi, float rms) {
        // a dip in loudness within a note: sung again (repeated notes)
        if (!Float.isNaN(midi) && !note.isEmpty() && rms < notePeak * 0.35f) midi = Float.NaN;
        if (Float.isNaN(midi)) {
            jump.clear();
            if (++gap >= GAP) endNote();
            if (++quiet >= PHRASE_END && pauseEnds) endPhrase();
            return;
        }
        quiet = 0;
        gap = 0;
        if (note.isEmpty()) {
            note.add(midi);
            notePeak = rms;
            return;
        }
        float m = median(note);
        if (Math.abs(midi - m) > 1.0f) {  // another pitch: a new note once it holds
            jump.add(midi);
            if (jump.size() >= JUMP) {
                java.util.ArrayList<Float> next = new java.util.ArrayList<>(jump);
                jump.clear();
                endNote();
                note.addAll(next);
                notePeak = rms;
            }
            return;
        }
        note.addAll(jump);
        jump.clear();
        note.add(midi);
        notePeak = Math.max(notePeak, rms);
        if (!noteHeld && note.size() >= MIN_NOTE) {
            noteHeld = true;
            float[] p = new float[phrase.size() + 1];
            for (int i = 0; i < phrase.size(); i++) p[i] = phrase.get(i);
            p[p.length - 1] = median(note);
            listener.onNote(p);
        }
    }

    private void endNote() {
        if (note.size() >= MIN_NOTE) {
            phrase.add(median(note));
            if (phrase.size() > MAX_NOTES) phrase.remove(0);
            listener.onNotes(phrase.size());
        }
        note.clear();
        notePeak = 0;
        noteHeld = false;
    }

    private void endPhrase() {
        endNote();
        if (phrase.isEmpty()) return;
        float[] p = new float[phrase.size()];
        for (int i = 0; i < p.length; i++) p[i] = phrase.get(i);
        phrase.clear();
        listener.onNotes(0);
        listener.onPhrase(p.length >= 3 ? match(p, beat) : -1, p);
    }

    private static float median(java.util.List<Float> l) {
        float[] a = new float[l.size()];
        for (int i = 0; i < a.length; i++) a[i] = l.get(i);
        java.util.Arrays.sort(a);
        return a[a.length / 2];
    }

    // ---- songs
    /** the song these notes are (-1 none): the closest form of the same length, in any key; it must fit well and
     *  clearly better than any other song */
    static int match(float[] p) { return match(p, 0); }

    /** beat: only songs in that time (3, 4, 6), 0 any */
    static int match(float[] p, int beat) {
        int song = matchForms(p, beat);
        if (song >= 0) return song;
        // a held note sung with a wavering voice comes out as the same pitch twice: run those together
        int n = 0;
        float[] merged = new float[p.length];
        for (int i = 0; i < p.length; i++)
            if (n == 0 || Math.abs(p[i] - merged[n - 1]) >= 0.8f) merged[n++] = p[i];
        return n < p.length && n >= 3 ? matchForms(java.util.Arrays.copyOf(merged, n), beat) : -1;
    }

    private static int matchForms(float[] p, int beat) {
        int best = -1;
        float bestErr = Float.MAX_VALUE, otherErr = Float.MAX_VALUE;
        float[] err = new float[SONG_COUNT];
        java.util.Arrays.fill(err, Float.MAX_VALUE);
        for (int f = 0; f < forms.size(); f++) {
            int[] s = forms.get(f);
            if (s.length != p.length) continue;
            float shift = 0;
            for (int i = 0; i < s.length; i++) shift += p[i] - s[i];
            shift /= s.length;
            float sq = 0, max = 0;
            for (int i = 0; i < s.length; i++) {
                float r = Math.abs(p[i] - s[i] - shift);
                sq += r * r;
                max = Math.max(max, r);
            }
            float rms = (float) Math.sqrt(sq / s.length);
            if (max > 1.8f) continue;
            int song = formSong.get(f);
            if (beat != 0 && SONGS[song][0] != beat) continue;
            err[song] = Math.min(err[song], rms);
        }
        for (int s = 0; s < SONG_COUNT; s++)
            if (err[s] < bestErr) {
                otherErr = bestErr;
                bestErr = err[s];
                best = s;
            } else if (err[s] < otherErr) {
                otherErr = err[s];
            }
        if (best < 0 || bestErr > 1.0f || otherErr - bestErr < 0.3f) return -1;
        return best;
    }

    // ---- conducting by voice: the direction (C, U, R, D, L = the game's c_pos 0..4) the C stick gives the game for
    // the note sung last. Each beat the game takes one note, and a song is as many notes as its time has beats, so
    // the notes count from the phrase's start in groups of that many. The key is the singer's own: the notes so far
    // against the start of each song in this time (Link's learned ones first: learned = mTact bits), the best fit
    // gives the direction; nothing fitting: the nearest note of the time's scale in the best key. The first note
    // of a group: with the singer's middle note (ref, hummed in the count-in; NaN none) the song whose first note
    // lies nearest to it when ref is the middle of the time's notes (learned songs only, if any), and later notes
    // fit better the nearer their key stays to that; without it a guess (the first such song's first note).
    static int direction(float[] notes, int beat, int learned) { return direction(notes, beat, learned, null, Float.NaN); }

    /** how many notes these are for a song in this time: as direction() counts them (a note held with a wavering
     *  voice once, but in 6/4, whose songs repeat notes) */
    static int noteCount(float[] notes, int beat) {
        if (beat == 6) return notes.length;
        int n = 0;
        for (int i = 0; i < notes.length; i++) if (i == 0 || Math.abs(notes[i] - notes[i - 1]) >= 0.8f) n++;
        return n;
    }

    /** the middle (MIDI) of a table's notes but C: where a singer's middle note goes */
    static float middle(int[] tbl) {
        int lo = Integer.MAX_VALUE, hi = Integer.MIN_VALUE;
        for (int d = 1; d < tbl.length; d++) {
            lo = Math.min(lo, tbl[d]);
            hi = Math.max(hi, tbl[d]);
        }
        return (lo + hi) / 2f;
    }

    private static final float REF_WEIGHT = 0.1f;  // a key 3 semitones off the middle note's costs 0.9 (squared semitones)

    /** key (may be null) gets the key that direction is in: [0] the shift (MIDI) onto scale(beat, [1]) */
    static int direction(float[] notes, int beat, int learned, float[] key, float ref) {
        boolean hasRef = !Float.isNaN(ref);
        if (beat != 6) {  // no song in 3/4 or 4/4 repeats a note: one held with a wavering voice counts once
            int n = 0;
            float[] merged = new float[notes.length];
            for (float v : notes) if (n == 0 || Math.abs(v - merged[n - 1]) >= 0.8f) merged[n++] = v;
            notes = java.util.Arrays.copyOf(merged, n);
        }
        int k = notes.length - 1;
        if (k < 0 || beat == 0) return C;
        int from = k - k % beat, j = k - from;
        int best = -1, firstSong = -1, firstLearned = -1;
        float bestErr = Float.MAX_VALUE;
        for (int s = 0; s < SONGS.length; s++) {
            if (SONGS[s][0] != beat) continue;
            boolean known = (learned & (1 << s)) != 0;
            if (firstSong < 0) firstSong = s;
            if (known && firstLearned < 0) firstLearned = s;
            int[] tbl = table(SONGS[s]);
            float shift = 0;
            for (int i = 0; i <= j; i++) shift += notes[from + i] - tbl[SONGS[s][1 + i]];
            shift /= j + 1;
            float max = 0, sq = 0;
            for (int i = 0; i <= j; i++) {
                float r = Math.abs(notes[from + i] - tbl[SONGS[s][1 + i]] - shift);
                max = Math.max(max, r);
                sq += r * r;
            }
            if (max > 1.8f) continue;
            float e = sq - (known ? 0.01f : 0);  // (a tie, the first note above all: the learned song)
            if (hasRef) e += REF_WEIGHT * (shift - (ref - middle(tbl))) * (shift - (ref - middle(tbl)));
            if (e < bestErr) {
                bestErr = e;
                best = s;
            }
        }
        if (j == 0 && hasRef) {  // the first note against the middle note
            float near = Float.MAX_VALUE;
            for (int s = 0; s < SONGS.length; s++) {
                if (SONGS[s][0] != beat || firstLearned >= 0 && (learned & (1 << s)) == 0) continue;
                int[] tbl = table(SONGS[s]);
                float r = Math.abs(notes[from] - (ref - middle(tbl)) - tbl[SONGS[s][1]]);
                if (r < near) {
                    near = r;
                    best = s;
                }
            }
        } else if (j == 0) best = firstLearned >= 0 ? firstLearned : firstSong;
        if (best >= 0) {
            if (key != null) {
                int[] tbl = table(SONGS[best]);
                float shift = 0;
                for (int i = 0; i <= j; i++) shift += notes[from + i] - tbl[SONGS[best][1 + i]];
                key[0] = shift / (j + 1);
                key[1] = tbl == BEAT62 ? 1 : 0;
            }
            return SONGS[best][1 + j];
        }
        // no song's start: the time's scale, in the singer's key (from the middle note) or the one that fits best
        int[] tbl = beat == 3 ? BEAT3 : beat == 4 ? BEAT4 : BEAT61;
        if (hasRef) {
            float shift = ref - middle(tbl), near = Float.MAX_VALUE;
            int dir = C;
            for (int d = 0; d < tbl.length; d++)
                if (Math.abs(notes[k] - shift - tbl[d]) < near) {
                    near = Math.abs(notes[k] - shift - tbl[d]);
                    dir = d;
                }
            if (key != null) {
                key[0] = shift;
                key[1] = 0;
            }
            return dir;
        }
        int dir = C;
        float bestSum = Float.MAX_VALUE;
        for (int anchor = 0; anchor < tbl.length; anchor++) {
            float shift = notes[from] - tbl[anchor], sum = 0;
            int last = C;
            for (int i = from; i <= k; i++) {
                float near = Float.MAX_VALUE;
                for (int d = 0; d < tbl.length; d++) {
                    float r = Math.abs(notes[i] - shift - tbl[d]);
                    if (r < near) {
                        near = r;
                        if (i == k) last = d;
                    }
                }
                sum += near * near;
            }
            if (sum < bestSum) {
                bestSum = sum;
                dir = last;
                if (key != null) {
                    key[0] = shift;
                    key[1] = 0;
                }
            }
        }
        return dir;
    }

    /** the pitches (MIDI, before the singer's key) of the directions C U R D L in this time; alt: 6/4 begun up */
    static int[] scale(int beat, boolean alt) {
        return beat == 3 ? BEAT3 : beat == 4 ? BEAT4 : alt ? BEAT62 : BEAT61;
    }

    private static int[] table(int[] song) {
        return song[0] == 3 ? BEAT3 : song[0] == 4 ? BEAT4 : song[1] == U ? BEAT62 : BEAT61;
    }

    /** the song's time (3, 4, 6) */
    static int beatOf(int song) { return SONGS[song][0]; }

    /** the song's notes (MIDI), as the game plays them while conducting (tests) */
    static int[] notesOf(int song) {
        for (int f = 0; f < forms.size(); f++)
            if (formSong.get(f) == song) return forms.get(f);
        return new int[0];
    }
}
