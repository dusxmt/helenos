/*
 * Copyright (c) 2006 Martin Decky
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/** @addtogroup test
 * @{
 */
/** @file
 */

#ifndef KERN_TEST_H_
#define KERN_TEST_H_

#include <arch/types.h>
#include <typedefs.h>

typedef char *(*test_entry_t)(bool);

typedef struct {
	char *name;
	char *desc;
	test_entry_t entry;
	bool safe;
} test_t;

extern char * test_atomic1(bool quiet);
extern char * test_avltree1(bool quiet);
extern char * test_btree1(bool quiet);
extern char * test_mips1(bool quiet);
extern char * test_fault1(bool quiet);
extern char * test_fpu1(bool quiet);
extern char * test_sse1(bool quiet);
extern char * test_mips2(bool quiet);
extern char * test_falloc1(bool quiet);
extern char * test_falloc2(bool quiet);
extern char * test_mapping1(bool quiet);
extern char * test_purge1(bool quiet);
extern char * test_slab1(bool quiet);
extern char * test_slab2(bool quiet);
extern char * test_rwlock1(bool quiet);
extern char * test_rwlock2(bool quiet);
extern char * test_rwlock3(bool quiet);
extern char * test_rwlock4(bool quiet);
extern char * test_rwlock5(bool quiet);
extern char * test_semaphore1(bool quiet);
extern char * test_semaphore2(bool quiet);
extern char * test_print1(bool quiet);
extern char * test_thread1(bool quiet);
extern char * test_sysinfo1(bool quiet);

extern test_t tests[];

#endif

/** @}
 */
