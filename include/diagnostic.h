#ifndef DIAGNOSTIC_H
#define DIAGNOSTIC_H

#ifdef DIAGNOSTIC
void diagnostic_reset(void);
void diagnostic_record(const char *stage, int result);
#else
static inline void diagnostic_reset(void)
{
}

static inline void diagnostic_record(const char *stage, int result)
{
	(void)stage;
	(void)result;
}
#endif

#endif
