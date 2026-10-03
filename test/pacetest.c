// SPDX-License-Identifier: BSD-3-Clause
// Tests for multiplier = auto (framegen.c's pace()) against a simulated game and FIFO display.
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
    float refresh;    // the "refresh" setting, 0 to measure it
    double fps;       // the most the game renders
    int max_shown;    // frames a present can show (spare images), 0 for no limit
    double t;
    struct swapchain sc;
    double at[MAX_GEN + 2];  // seconds spent at each multiplier
    int changes, downs;      // multiplier changes, and those down
};

static void sim_init(struct sim *s, double hz, float refresh, double fps)
{
    memset(s, 0, sizeof(*s));
    s->hz = hz;
    s->refresh = refresh;
    s->fps = fps;
    s->t = 100.0;
}

// Runs until `until` seconds into the simulation; returns the multiplier then
static int sim_run(struct sim *s, double until)
{
    int n = 0;
    while (s->t < 100.0 + until) {
        int prev = s->sc.pace.n;
        n = pace(&s->sc, s->t, s->refresh);
        if (prev && n != prev) {
            s->changes++;
            s->downs += n < prev;
        }
        int shown = s->max_shown && n > s->max_shown ? s->max_shown : n;
        double game = 1.0 / s->fps, shown_t = shown / s->hz;
        double dt = game > shown_t ? game : shown_t;
        s->sc.pace.waited_ns += (uint64_t)((dt - game) * 1e9);
        s->sc.pace.win_shown += shown;
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

    printf("%d of %d pacing tests passed\n", run_n - failed, run_n);
    return failed != 0;
}
