#ident "$Id: cachetable-simple-verify.c 34757 2011-09-14 19:12:42Z leifwalsh $"
#ident "Copyright (c) 2007-2011 Tokutek Inc.  All rights reserved."
#include "includes.h"
#include "test.h"

BOOL foo;
BOOL is_fake_locked;

//
// This test verifies that get_and_pin_nonblocking works and returns DB_TRYAGAIN when the PAIR is being used.
//

static void
flush (CACHEFILE f __attribute__((__unused__)),
       int UU(fd),
       CACHEKEY k  __attribute__((__unused__)),
       void *v     __attribute__((__unused__)),
       void *e     __attribute__((__unused__)),
       long s      __attribute__((__unused__)),
        long* new_size      __attribute__((__unused__)),
       BOOL w      __attribute__((__unused__)),
       BOOL keep   __attribute__((__unused__)),
       BOOL c      __attribute__((__unused__))
       ) {
  /* Do nothing */
  if (verbose) { printf("FLUSH: %d\n", (int)k.b); }
  // this should not be flushed until the bottom of the test, which
  // verifies that this is called if we have it pending a checkpoint
  if (w) {
    assert(c);
    assert(keep);
  }
  //usleep (5*1024*1024);
}

static int
fetch (CACHEFILE f        __attribute__((__unused__)),
       int UU(fd),
       CACHEKEY k         __attribute__((__unused__)),
       u_int32_t fullhash __attribute__((__unused__)),
       void **value       __attribute__((__unused__)),
       long *sizep        __attribute__((__unused__)),
       int  *dirtyp,
       void *extraargs    __attribute__((__unused__))
       ) {
  *dirtyp = 0;
  *value = NULL;
  *sizep = 8;
  return 0;
}

static void 
pe_est_callback(
    void* UU(brtnode_pv), 
    long* bytes_freed_estimate, 
    enum partial_eviction_cost *cost, 
    void* UU(write_extraargs)
    )
{
    *bytes_freed_estimate = 0;
    *cost = PE_CHEAP;
}

static int 
pe_callback (
    void *brtnode_pv __attribute__((__unused__)), 
    long bytes_to_free __attribute__((__unused__)), 
    long* bytes_freed, 
    void* extraargs __attribute__((__unused__))
    ) 
{
    *bytes_freed = bytes_to_free;
    return 0;
}

static BOOL pf_req_callback(void* UU(brtnode_pv), void* UU(read_extraargs)) {
  return FALSE;
}

static BOOL true_pf_req_callback(void* UU(brtnode_pv), void* UU(read_extraargs)) {
  return TRUE;
}


static int pf_callback(void* UU(brtnode_pv), void* UU(read_extraargs), int UU(fd), long* UU(sizep)) {
  assert(FALSE);
}

static int true_pf_callback(void* UU(brtnode_pv), void* UU(read_extraargs), int UU(fd), long* sizep) {
    *sizep = 8;
    return 0;
}


static void kibbutz_work(void *fe_v)
{
    CACHEFILE f1 = fe_v;
    sleep(2);
    foo = TRUE;
    int r = toku_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, 8);
    assert(r==0);
    notify_cachefile_that_kibbutz_job_finishing(f1);    
}

static void fake_ydb_lock(void) {
    assert(!is_fake_locked);
    is_fake_locked = TRUE;
}

static void fake_ydb_unlock(void) {
    assert(is_fake_locked);
    is_fake_locked = FALSE;
}

static void
run_test (void) {
    is_fake_locked = TRUE;
    const int test_limit = 12;
    int r;
    CACHETABLE ct;
    r = toku_create_cachetable(&ct, test_limit, ZERO_LSN, NULL_LOGGER); assert(r == 0);
    char fname1[] = __FILE__ "test1.dat";
    unlink(fname1);
    CACHEFILE f1;
    r = toku_cachetable_openf(&f1, ct, fname1, O_RDWR|O_CREAT, S_IRWXU|S_IRWXG|S_IRWXO); assert(r == 0);

    toku_cachetable_set_lock_unlock_for_io(ct, fake_ydb_lock, fake_ydb_unlock);
    
    void* v1;
    long s1;
    //
    // test that if we are getting a PAIR for the first time that TOKUDB_TRY_AGAIN is returned
    // because the PAIR was not in the cachetable.
    //
    is_fake_locked = TRUE;
    r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, &s1, flush, fetch, pe_est_callback, pe_callback, pf_req_callback, pf_callback, NULL, NULL, NULL);
    assert(r==TOKUDB_TRY_AGAIN);
    assert(is_fake_locked);
    // now it should succeed
    r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, &s1, flush, fetch, pe_est_callback, pe_callback, pf_req_callback, pf_callback, NULL, NULL, NULL);
    assert(r==0);
    assert(is_fake_locked);
    foo = FALSE;
    cachefile_kibbutz_enq(f1, kibbutz_work, f1);
    // because node is in use, should return TOKUDB_TRY_AGAIN
    assert(is_fake_locked);
    r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, &s1, flush, fetch, pe_est_callback, pe_callback, pf_req_callback, pf_callback, NULL, NULL, NULL);
    assert(is_fake_locked);
    assert(r==TOKUDB_TRY_AGAIN);
    r = toku_cachetable_get_and_pin(f1, make_blocknum(1), 1, &v1, &s1, flush, fetch, pe_est_callback, pe_callback, pf_req_callback, pf_callback, NULL, NULL);
    assert(foo);
    r = toku_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, 8); assert(r==0);

    // now make sure we get TOKUDB_TRY_AGAIN when a partial fetch is involved
    assert(is_fake_locked);
    // first make sure value is there
    r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, &s1, flush, fetch, pe_est_callback, pe_callback, pf_req_callback, pf_callback, NULL, NULL, NULL);
    assert(is_fake_locked);
    assert(r==0);
    r = toku_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_CLEAN, 8); assert(r==0);
    // now make sure that we get TOKUDB_TRY_AGAIN for the partial fetch
    r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, &s1, flush, fetch, pe_est_callback, pe_callback, true_pf_req_callback, true_pf_callback, NULL, NULL, NULL);
    assert(is_fake_locked);
    assert(r==TOKUDB_TRY_AGAIN);

    //
    // now test that if there is a checkpoint pending, 
    // first pin and unpin with dirty
    //
    r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, &s1, flush, fetch, pe_est_callback, pe_callback, pf_req_callback, pf_callback, NULL, NULL, NULL);
    assert(is_fake_locked);
    assert(r==0);
    r = toku_cachetable_unpin(f1, make_blocknum(1), 1, CACHETABLE_DIRTY, 8); assert(r==0);
    // this should mark the PAIR as pending
    r = toku_cachetable_begin_checkpoint(ct, NULL); assert(r == 0);
    r = toku_cachetable_get_and_pin_nonblocking(f1, make_blocknum(1), 1, &v1, &s1, flush, fetch, pe_est_callback, pe_callback, pf_req_callback, pf_callback, NULL, NULL, NULL);
    assert(is_fake_locked);
    assert(r==TOKUDB_TRY_AGAIN);
    fake_ydb_unlock();
    r = toku_cachetable_end_checkpoint(
        ct, 
        NULL, 
        fake_ydb_lock,
        fake_ydb_unlock,
        NULL,
        NULL
        );
    assert(r==0);
    
    
    
    toku_cachetable_verify(ct);
    r = toku_cachefile_close(&f1, 0, FALSE, ZERO_LSN); assert(r == 0 && f1 == 0);
    r = toku_cachetable_close(&ct); lazy_assert_zero(r);
    
    
}

int
test_main(int argc, const char *argv[]) {
  default_parse_args(argc, argv);
  run_test();
  return 0;
}