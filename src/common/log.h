#ifndef PSS_COMMON_LOG_H
#define PSS_COMMON_LOG_H

/* Timestamped, one-line-per-call logging to stderr: "<date time.ms> <tag>: msg". */
void pss_log_init(const char *tag);
void pss_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
