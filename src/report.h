#ifndef WHY_REPORT_H
#define WHY_REPORT_H
#include "attribution.h"
#include <stdio.h>
void why_report_sample(FILE *out, const WhyFrame *previous,
					   const WhyFrame *current, long hz, size_t sequence,
					   bool details);
void why_report_history(FILE *out, const WhyHistory *history, bool timeline);
void why_report_spikes(FILE *out, const WhyHistory *history, long hz);
void why_report_query(FILE *out, const WhyHistory *history, long hz,
					  unsigned seconds, uint64_t now);
#endif
