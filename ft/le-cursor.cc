/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*
COPYING CONDITIONS NOTICE:

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation, and provided that the
  following conditions are met:

      * Redistributions of source code must retain this COPYING
        CONDITIONS NOTICE, the COPYRIGHT NOTICE (below), the
        DISCLAIMER (below), the UNIVERSITY PATENT NOTICE (below), the
        PATENT MARKING NOTICE (below), and the PATENT RIGHTS
        GRANT (below).

      * Redistributions in binary form must reproduce this COPYING
        CONDITIONS NOTICE, the COPYRIGHT NOTICE (below), the
        DISCLAIMER (below), the UNIVERSITY PATENT NOTICE (below), the
        PATENT MARKING NOTICE (below), and the PATENT RIGHTS
        GRANT (below) in the documentation and/or other materials
        provided with the distribution.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
  02110-1301, USA.

COPYRIGHT NOTICE:

  TokuDB, Tokutek Fractal Tree Indexing Library.
  Copyright (C) 2007-2013 Tokutek, Inc.

DISCLAIMER:

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

UNIVERSITY PATENT NOTICE:

  The technology is licensed by the Massachusetts Institute of
  Technology, Rutgers State University of New Jersey, and the Research
  Foundation of State University of New York at Stony Brook under
  United States of America Serial No. 11/760379 and to the patents
  and/or patent applications resulting from it.

PATENT MARKING NOTICE:

  This software is covered by US Patent No. 8,185,551.

PATENT RIGHTS GRANT:

  "THIS IMPLEMENTATION" means the copyrightable works distributed by
  Tokutek as part of the Fractal Tree project.

  "PATENT CLAIMS" means the claims of patents that are owned or
  licensable by Tokutek, both currently or in the future; and that in
  the absence of this license would be infringed by THIS
  IMPLEMENTATION or by using or running THIS IMPLEMENTATION.

  "PATENT CHALLENGE" shall mean a challenge to the validity,
  patentability, enforceability and/or non-infringement of any of the
  PATENT CLAIMS or otherwise opposing any of the PATENT CLAIMS.

  Tokutek hereby grants to you, for the term and geographical scope of
  the PATENT CLAIMS, a non-exclusive, no-charge, royalty-free,
  irrevocable (except as stated in this section) patent license to
  make, have made, use, offer to sell, sell, import, transfer, and
  otherwise run, modify, and propagate the contents of THIS
  IMPLEMENTATION, where such license applies only to the PATENT
  CLAIMS.  This grant does not include claims that would be infringed
  only as a consequence of further modifications of THIS
  IMPLEMENTATION.  If you or your agent or licensee institute or order
  or agree to the institution of patent litigation against any entity
  (including a cross-claim or counterclaim in a lawsuit) alleging that
  THIS IMPLEMENTATION constitutes direct or contributory patent
  infringement, or inducement of patent infringement, then any rights
  granted to you under this License shall terminate as of the date
  such litigation is filed.  If you or your agent or exclusive
  licensee institute or order or agree to the institution of a PATENT
  CHALLENGE, then Tokutek may terminate any rights granted to you
  under this License.
*/

#ident "Copyright (c) 2010-2013 Tokutek Inc.  All rights reserved."
#ident "The technology is licensed by the Massachusetts Institute of Technology, Rutgers State University of New Jersey, and the Research Foundation of State University of New York at Stony Brook under United States of America Serial No. 11/760379 and to the patents and/or patent applications resulting from it."

#include "ft.h"
#include "ft-internal.h"
#include "le-cursor.h"

// A LE_CURSOR is a special purpose FT_CURSOR that:
//  - enables prefetching
//  - does not perform snapshot reads. it reads everything, including uncommitted.
//
// A LE_CURSOR is good for scanning a FT from beginning to end. Useful for hot indexing.

struct le_cursor {
    // TODO: remove DBs from the ft layer comparison function 
    // so this is never necessary
    // use a fake db for comparisons. 
    struct __toku_db fake_db;
    FT_CURSOR ft_cursor;
    bool neg_infinity; // true when the le cursor is positioned at -infinity (initial setting)
    bool pos_infinity; // true when the le cursor is positioned at +infinity (when _next returns DB_NOTFOUND)
};

int 
toku_le_cursor_create(LE_CURSOR *le_cursor_result, FT_HANDLE ft_handle, TOKUTXN txn) {
    int result = 0;
    LE_CURSOR MALLOC(le_cursor);
    if (le_cursor == NULL) {
        result = get_error_errno();
    }
    else {
        result = toku_ft_cursor(ft_handle, &le_cursor->ft_cursor, txn, false, false);
        if (result == 0) {
            // TODO move the leaf mode to the ft cursor constructor
            toku_ft_cursor_set_leaf_mode(le_cursor->ft_cursor);
            le_cursor->neg_infinity = true;
            le_cursor->pos_infinity = false;
            // zero out the fake DB. this is a rare operation so it's not too slow.
            memset(&le_cursor->fake_db, 0, sizeof(le_cursor->fake_db));
        }
    }

    if (result == 0) {
        *le_cursor_result = le_cursor;
    } else {
        toku_free(le_cursor);
    }

    return result;
}

void toku_le_cursor_close(LE_CURSOR le_cursor) {
    toku_ft_cursor_close(le_cursor->ft_cursor);
    toku_free(le_cursor);
}

// Move to the next leaf entry under the LE_CURSOR
// Success: returns zero, calls the getf callback with the getf_v parameter
// Failure: returns a non-zero error number
int 
toku_le_cursor_next(LE_CURSOR le_cursor, FT_GET_CALLBACK_FUNCTION getf, void *getf_v) {
    int result;
    if (le_cursor->pos_infinity) {
        result = DB_NOTFOUND;
    } else {
        le_cursor->neg_infinity = false;
        // TODO replace this with a non deprecated function. Which?
        result = toku_ft_cursor_get(le_cursor->ft_cursor, NULL, getf, getf_v, DB_NEXT);
        if (result == DB_NOTFOUND) {
            le_cursor->pos_infinity = true;
        }
    }
    return result;
}

bool
toku_le_cursor_is_key_greater(LE_CURSOR le_cursor, const DBT *key) {
    bool result;
    if (le_cursor->neg_infinity) {
        result = true;      // all keys are greater than -infinity
    } else if (le_cursor->pos_infinity) {
        result = false;     // all keys are less than +infinity
    } else {
        // get the comparison function and descriptor from the cursor's ft
        FT_HANDLE ft_handle = le_cursor->ft_cursor->ft_handle;
        ft_compare_func keycompare = toku_ft_get_bt_compare(ft_handle);
        le_cursor->fake_db.cmp_descriptor = toku_ft_get_cmp_descriptor(ft_handle);
        // get the current position from the cursor and compare it to the given key.
        DBT *cursor_key = &le_cursor->ft_cursor->key;
        int r = keycompare(&le_cursor->fake_db, cursor_key, key);
        if (r < 0) {
            result = true;  // key is right of the cursor key
        } else {
            result = false; // key is at or left of the cursor key
        }
    }
    return result;
}
