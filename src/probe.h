#ifndef SHELF_PROBE_H
#define SHELF_PROBE_H

// Appends a hardware report to `path` while showing a test pattern for about 17 s; needs
// platform_init. Returns 0, or -1 when the report can't be written.
int probe_run(const char *path);

#endif
