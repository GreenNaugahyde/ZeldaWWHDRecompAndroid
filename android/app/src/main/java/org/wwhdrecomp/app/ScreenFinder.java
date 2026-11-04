package org.wwhdrecomp.app;

import android.content.Context;
import android.net.nsd.NsdManager;
import android.net.nsd.NsdServiceInfo;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.ArrayList;
import java.util.List;

/**
 * Show on a TV: devices waiting to be a screen (the screen app, or "Be another device's screen") announce themselves
 * (PadLink.SCREEN_SERVICE_TYPE) with a small socket for an invitation; the console finds them here and invites one:
 * its address, port and a one-time pass (PadHost.ticket), with which the screen then joins it as it would a console it
 * found itself, without the code.
 */
final class ScreenFinder {
    private static final String TAG = "wwhd";

    static final class Screen {
        final String name;
        final InetAddress host;
        final int port;

        Screen(String name, InetAddress host, int port) {
            this.name = name;
            this.host = host;
            this.port = port;
        }
    }

    // ---- the console's side: finding them, inviting one
    private final NsdManager nsd;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final Runnable changed;
    private final List<Screen> found = new ArrayList<>();
    private final List<NsdServiceInfo> toResolve = new ArrayList<>();
    private boolean resolving;
    private NsdManager.DiscoveryListener discovery;

    /** `changed`: the list changed (main thread) */
    ScreenFinder(Context c, Runnable changed) {
        nsd = c.getSystemService(NsdManager.class);
        this.changed = changed;
    }

    List<Screen> found() { return new ArrayList<>(found); }

    void start() {
        if (discovery != null) return;
        discovery = new NsdManager.DiscoveryListener() {
            @Override public void onStartDiscoveryFailed(String type, int err) { Log.w(TAG, "screens: discovery failed " + err); }
            @Override public void onStopDiscoveryFailed(String type, int err) {}
            @Override public void onDiscoveryStarted(String type) {}
            @Override public void onDiscoveryStopped(String type) {}

            @Override
            public void onServiceFound(NsdServiceInfo info) {
                main.post(() -> {
                    toResolve.add(info);
                    resolveNext();
                });
            }

            @Override
            public void onServiceLost(NsdServiceInfo info) {
                main.post(() -> {
                    if (found.removeIf(s -> s.name.equals(info.getServiceName()))) changed.run();
                });
            }
        };
        nsd.discoverServices(PadLink.SCREEN_SERVICE_TYPE, NsdManager.PROTOCOL_DNS_SD, discovery);
    }

    void stop() {
        if (discovery == null) return;
        try {
            nsd.stopServiceDiscovery(discovery);
        } catch (IllegalArgumentException ignored) {
        }
        discovery = null;
        toResolve.clear();
    }

    @SuppressWarnings("deprecation")
    private void resolveNext() {
        if (resolving || toResolve.isEmpty() || discovery == null) return;
        resolving = true;
        nsd.resolveService(toResolve.remove(0), new NsdManager.ResolveListener() {
            @Override
            public void onResolveFailed(NsdServiceInfo info, int err) {
                main.post(() -> {
                    resolving = false;
                    resolveNext();
                });
            }

            @Override
            public void onServiceResolved(NsdServiceInfo info) {
                boolean self = PadActivity.isOwnAddress(info.getHost());
                main.post(() -> {
                    resolving = false;
                    if (!self && discovery != null) {
                        found.removeIf(s -> s.name.equals(info.getServiceName()));
                        found.add(new Screen(info.getServiceName(), info.getHost(), info.getPort()));
                        changed.run();
                    }
                    resolveNext();
                });
            }
        });
    }

    /** the invitation (a thread of its own): true if the screen took it */
    static boolean invite(Screen s, String console, int port, String ticket) {
        try (Socket so = new Socket()) {
            so.connect(new InetSocketAddress(s.host, s.port), 3000);
            so.setSoTimeout(3000);
            DataOutputStream o = new DataOutputStream(so.getOutputStream());
            o.writeUTF(PadLink.INVITE);
            o.writeInt(PadLink.VERSION);
            o.writeUTF(console);
            o.writeInt(port);
            o.writeUTF(ticket);
            o.flush();
            return new DataInputStream(so.getInputStream()).readBoolean();
        } catch (IOException e) {
            Log.w(TAG, "screens: inviting " + s.name + " failed: " + e);
            return false;
        }
    }

    // ---- the screen's side: announced, waiting for an invitation
    interface Invited {
        /** a console invites this screen (main thread): true if it goes there */
        boolean invited(String console, InetAddress host, int port, String ticket);
    }

    static final class Waiting {
        private final NsdManager nsd;
        private final Handler main = new Handler(Looper.getMainLooper());
        private volatile ServerSocket server;
        private volatile NsdManager.RegistrationListener registration;

        Waiting(Context c, String name, Invited invited) {
            nsd = c.getSystemService(NsdManager.class);
            new Thread(() -> serve(name, invited), "wwhd-screen-wait").start();
        }

        private void serve(String name, Invited invited) {
            ServerSocket ss;
            try {
                ss = new ServerSocket(0);
            } catch (IOException e) {
                Log.w(TAG, "screen: no socket for invitations", e);
                return;
            }
            server = ss;
            NsdServiceInfo info = new NsdServiceInfo();
            info.setServiceName(name);
            info.setServiceType(PadLink.SCREEN_SERVICE_TYPE);
            info.setPort(ss.getLocalPort());
            registration = new NsdManager.RegistrationListener() {
                @Override public void onServiceRegistered(NsdServiceInfo i) { Log.i(TAG, "screen: waiting as " + i.getServiceName()); }
                @Override public void onRegistrationFailed(NsdServiceInfo i, int err) { Log.w(TAG, "screen: announcing failed " + err); }
                @Override public void onServiceUnregistered(NsdServiceInfo i) {}
                @Override public void onUnregistrationFailed(NsdServiceInfo i, int err) {}
            };
            nsd.registerService(info, NsdManager.PROTOCOL_DNS_SD, registration);
            if (server != ss) stop();  // (stopped meanwhile)
            while (server == ss) {
                try (Socket s = ss.accept()) {
                    s.setSoTimeout(3000);
                    DataInputStream in = new DataInputStream(s.getInputStream());
                    if (!PadLink.INVITE.equals(in.readUTF())) continue;
                    int version = in.readInt();
                    String console = in.readUTF();
                    int port = in.readInt();
                    String ticket = in.readUTF();
                    InetAddress host = s.getInetAddress();
                    boolean[] ok = {false};
                    if (version == PadLink.VERSION) {
                        java.util.concurrent.CountDownLatch done = new java.util.concurrent.CountDownLatch(1);
                        main.post(() -> {
                            ok[0] = invited.invited(console, host, port, ticket);
                            done.countDown();
                        });
                        try {
                            done.await(2, java.util.concurrent.TimeUnit.SECONDS);
                        } catch (InterruptedException ignored) {
                        }
                    }
                    Log.i(TAG, "screen: invited by " + console + (ok[0] ? "" : " (not taken)"));
                    DataOutputStream o = new DataOutputStream(s.getOutputStream());
                    o.writeBoolean(ok[0]);
                    o.flush();
                } catch (IOException e) {
                    if (server != ss) break;
                }
            }
        }

        void stop() {
            ServerSocket ss = server;
            server = null;
            if (registration != null) {
                try {
                    nsd.unregisterService(registration);
                } catch (IllegalArgumentException ignored) {
                }
                registration = null;
            }
            if (ss != null) try {
                ss.close();
            } catch (IOException ignored) {
            }
        }
    }
}
