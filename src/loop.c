#include "loop.h"

#define FRAME (1.0 / 60)
#define MAX_DT .1f // bound catch-up after a stall

int loop_run(const LoopIO *io, const LoopScreen *s, Image *canvas, LoopStats *stats)
{
    LoopStats local;
    if (!stats) stats = &local;
    double last = io->now(), next_frame = last;
    for (;;) {
        Button b;
        while ((b = io->poll()) != BTN_NONE) s->button(s->ud, b);
        double now = io->now();
        double tick = s->tick ? s->tick(s->ud, now) : now + 3600;
        float dt = (float)(now - last);
        last = now;
        s->update(s->ud, dt < 0 ? 0 : dt > MAX_DT ? MAX_DT : dt);
        if (s->idle(s->ud) <= 0) {
            s->draw(s->ud, canvas);
            io->present(canvas);
            stats->frames++;
        }
        int done = s->after(s->ud);
        if (done) return done;

        now = io->now();
        double rest = s->idle(s->ud);
        if (rest > 0) {
            double until = now + rest < tick ? now + rest : tick;
            io->wait(until);
            stats->waits++;
            last = io->now(); // time asleep isn't animation time
            next_frame = last;
        } else if (!io->paced) {
            // Present already waits on vblank when paced; sleeping as well would push
            // most frames past the next one.
            next_frame += FRAME;
            if (next_frame < now - MAX_DT) next_frame = now; // fell behind; don't spiral
            io->wait(next_frame < tick ? next_frame : tick);
        }
    }
}
