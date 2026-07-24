#ifndef BLUETUI_LOG_H
#define BLUETUI_LOG_H

/* Lightweight file logger. Active only when the environment variable
 * BLUETUI_LOG points at a writable path; otherwise every call is a no-op. */
void log_init(void);
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_close(void);

#endif /* BLUETUI_LOG_H */
