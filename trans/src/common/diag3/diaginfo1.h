/* $Id$ */

/*
 * Copyright 2002-2011, The TenDRA Project.
 * Copyright 1997, United Kingdom Secretary of State for Defence.
 *
 * See doc/copyright/ for the full copyright terms.
 */

#ifndef diaginfo1_key
#define diaginfo1_key 1

typedef enum {
	DIAG_INFO_UNINIT,
	DIAG_INFO_SOURCE,
	DIAG_INFO_ID,
	DIAG_INFO_TYPE,
	DIAG_INFO_TAG
} diag_info_key;

struct diag_info_t {
	diag_info_key key;
	union {
		struct {
			sourcemark beg;
			sourcemark end;
		} source;
		struct {
			tdfstring nme;
			exp access;
			diag_type typ;
		} id_scope;
		struct {
			tdfstring nme;
			diag_type typ;
		} type_scope;
		struct {
			tdfstring nme;
			diag_type typ;
		} tag_scope;
	} data;
};

				/* end phase 2 */
#endif

