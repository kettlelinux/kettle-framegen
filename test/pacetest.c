// SPDX-License-Identifier: BSD-3-Clause
// Tests for multiplier = auto (framegen.c's pace()) and min_fps against a simulated game and FIFO
// display.
//
//   pacetest [-v]
//
// The game renders at most `fps` frames per second. Each present shows the multiplier's frames
// (or fewer, where the swapchain runs short of images); FIFO shows one per refresh, so when the
// display is full the game waits for it and runs at refresh / shown. That wait is what the
// layer measures. The game's frames and the waits are fed to pace() as the layer does, on a
// simulated clock. -v shows the layer's log.
#include "../framegen.c"

struct sim {
    double hz;        // the display's refresh rate
    struct config cfg;  // multiplier (0: auto), refresh (0: measure it), min_fps
    double fps;       // the most the game renders
    int max_shown;    // frames a present can show (spare images), 0 for no limit
    double t;
    struct swapchain sc;
    double at[MAX_GEN + 2];  // seconds spent at each multiplier
    int n;                   // the multiplier last returned
    int changes, downs;      // multiplier changes, and those down
};

static void sim_init(struct sim *s, double hz, float refresh, double fps)
{
    memset(s, 0, sizeof(*s));
    s->hz = hz;
    s->cfg.refresh = refresh;
    s->fps = fps;
    s->t = 100.0;
}

// Runs until `until` seconds into the simulation; returns the multiplier then
static int sim_run(struct sim *s, double until)
{
    int n = 0;
    while (s->t < 100.0 + until) {
        n = multiplier_now(&s->sc, s->t, &s->cfg);
        if (s->n && n != s->n) {
            s->changes++;
            s->downs += n < s->n;
        }
        s->n = n;
        int shown = s->max_shown && n > s->max_shown ? s->max_shown : n;
        double game = 1.0 / s->fps, shown_t = shown / s->hz;
        double dt = game > shown_t ? game : shown_t;
        s->sc.pace.waited_ns += (uint64_t)((dt - game) * 1e9);
        s->sc.pace.win_shown += shown;
        s->sc.floor.waited_ns += (uint64_t)((dt - game) * 1e9);
        s->sc.floor.win_shown += shown;
        s->at[n] += dt;
        s->t += dt;
    }
    return n;
}

static double share(const struct sim *s, int n)
{
    double all = 0;
    for (int i = 0; i <= MAX_GEN + 1; i++)
        all += s->at[i];
    return s->at[n] / all;
}

static int failed, run_n;

static void check(const char *name, bool ok, const struct sim *s)
{
    run_n++;
    failed += !ok;
    printf("%-44s %s  (at 1x %3.0f%%, 2x %3.0f%%, 3x %3.0f%%, %d changes)\n", name, ok ? "ok  " : "FAIL",
           100 * share(s, 1), 100 * share(s, 2), 100 * share(s, 3), s->changes);
}

int main(int argc, char **argv)
{
    if (!(argc > 1 && !strcmp(argv[1], "-v")) && !freopen("/dev/null", "w", stderr))
        return 1;
    struct sim s;

    // 70 fps on 180 Hz needs 3x; 2x is tried now and then, ever less often
    sim_init(&s, 180, 180, 70);
    int n = sim_run(&s, 600);
    check("70 fps at 180 Hz: 3x", n == 3 && share(&s, 3) > 0.97 && s.downs <= 7, &s);

    // without the refresh setting, measured while the game waits for the display
    sim_init(&s, 180, 0, 70);
    n = sim_run(&s, 600);
    check("70 fps at 180 Hz, refresh measured: 3x", n == 3 && share(&s, 3) > 0.95, &s);

    // 100 fps fills 180 Hz at 2x; 1x is tried and given up
    sim_init(&s, 180, 180, 100);
    n = sim_run(&s, 600);
    check("100 fps at 180 Hz: 2x", n == 2 && share(&s, 2) > 0.97 && s.downs <= 7, &s);

    // faster than the display: no generated frames needed
    sim_init(&s, 180, 180, 250);
    n = sim_run(&s, 120);
    check("250 fps at 180 Hz: 1x", n == 1 && s.changes == 1, &s);

    // 2x held the game at 90 fps; when it can do more, 1x is found by trying
    sim_init(&s, 120, 120, 50);
    n = sim_run(&s, 60);
    bool was3 = n == 3;
    s.fps = 130;
    n = sim_run(&s, 300);
    check("50 then 130 fps at 120 Hz: 3x, then 1x", was3 && n == 1, &s);

    // the game slows from 100 to 55 fps: up to 3x within a few seconds
    sim_init(&s, 180, 180, 100);
    n = sim_run(&s, 60);
    bool was2 = n == 2;
    s.fps = 55;
    n = sim_run(&s, 63);
    check("100 then 55 fps at 180 Hz: 3x within 3 s", was2 && n == 3, &s);

    // the swapchain has spare images for one generated frame only: 3x shows no more than 2x,
    // so it is undone, and retried ever less often
    sim_init(&s, 180, 180, 70);
    s.max_shown = 2;
    n = sim_run(&s, 600);
    check("70 fps at 180 Hz, images for 2x: 2x", n == 2 && share(&s, 3) < 0.05 &&
          s.sc.pace.cap_wait > PACE_TRY, &s);

    // min_fps: a game too slow for it gets no generated frames, within a few seconds
    sim_init(&s, 60, 60, 25);
    s.cfg.min_fps = 30;
    n = sim_run(&s, 60);
    check("25 fps at 60 Hz, min_fps 30: 1x", n == 1 && share(&s, 1) > 0.9 && s.changes <= 2, &s);

    // ...and gets them back when it speeds up; the auto multiplier carries on from there
    s.fps = 50;
    n = sim_run(&s, 180);
    check("25 then 50 fps at 60 Hz, min_fps 30: then 2x", n == 2, &s);

    // the same where the refresh rate is measured (no present timing, no refresh setting)
    sim_init(&s, 60, 0, 25);
    s.cfg.min_fps = 30;
    n = sim_run(&s, 60);
    check("25 fps at 60 Hz, refresh measured, min_fps 30: 1x", n == 1 && share(&s, 1) > 0.8, &s);

    // auto doesn't hold a game below the floor: 3x would hold it at 20 fps on 60 Hz
    sim_init(&s, 60, 60, 40);
    s.cfg.min_fps = 30;
    n = sim_run(&s, 120);
    check("40 fps at 60 Hz, min_fps 30: 2x, never 3x", n == 2 && share(&s, 3) == 0, &s);

    // just above the floor: generation stays on
    sim_init(&s, 75, 75, 31);
    s.cfg.multiplier = 2;
    s.cfg.min_fps = 30;
    n = sim_run(&s, 60);
    check("31 fps at 75 Hz, 2x, min_fps 30: 2x", n == 2 && s.changes == 0, &s);

    // off below the floor, and not back on until a tenth above it
    s.fps = 27;
    bool off = sim_run(&s, 90) == 1;
    s.fps = 32;
    bool stays = sim_run(&s, 120) == 1;
    s.fps = 34;
    n = sim_run(&s, 150);
    check("31, 27, 32, 34 fps at 75 Hz, 2x, min_fps 30: 2x, 1x, 1x, 2x", off && stays && n == 2 && s.changes == 2, &s);

    // with a fixed multiplier and the refresh rate measured
    sim_init(&s, 60, 0, 25);
    s.cfg.multiplier = 2;
    s.cfg.min_fps = 30;
    n = sim_run(&s, 60);
    check("25 fps at 60 Hz, 2x, refresh measured, min_fps 30: 1x", n == 1 && s.changes == 1, &s);

    // 3x holds a faster game at 20 fps: it could go faster, so the floor doesn't apply
    sim_init(&s, 60, 0, 100);
    s.cfg.multiplier = 3;
    s.cfg.min_fps = 30;
    n = sim_run(&s, 60);
    check("100 fps at 60 Hz, 3x (held at 20 fps), min_fps 30: 3x", n == 3 && s.changes == 0, &s);

    printf("%d of %d pacing tests passed\n", run_n - failed, run_n);
    return failed != 0;
}
