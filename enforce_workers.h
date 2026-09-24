/*-------------------------------------------------------------------------
 *
 * enforce_workers.h
 *		Internal header for the enforce_workers module.
 *
 * The module's SQL footprint is one function, installed by CREATE EXTENSION
 * and used only by seqguard.c, so this header is not installed.  It exists
 * only so that _PG_init() in enforce_workers.c can reach the per-feature
 * initialisers that live in the other files.
 *
 *-------------------------------------------------------------------------
 */
#ifndef ENFORCE_WORKERS_H
#define ENFORCE_WORKERS_H

/* nlguard.c */
extern void nlguard_init(void);

/* seqguard.c */
extern void seqguard_init(void);

/* idxdefer.c */
extern void idxdefer_init(void);

#endif							/* ENFORCE_WORKERS_H */
