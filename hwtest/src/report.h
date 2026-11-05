#ifndef E1_REPORT_H
#define E1_REPORT_H

/* Device identity, used by the summary page and the result file. */
void report_ids(char *cpu, int cpu_len, char *sn, int sn_len);

/* Writes /mnt/sdcard/e1hwtest/results/<cpu>-<ts>.json and appends
 * /mnt/sdcard/e1hwtest/results.csv. Safe to call more than once. */
void report_write(void);

#endif
