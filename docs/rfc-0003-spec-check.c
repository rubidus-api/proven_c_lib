/*
 * docs/rfc-0003-spec-check.c - executes the specification in
 * docs/RFC-0003-implementing-the-view-vocabulary.md against its own tables.
 *
 * This file is NOT part of the library and is not built by ./nob. proven_u8str_view_split does
 * not exist yet; section 3.1 of the RFC specifies it as four ordered steps, and this file is
 * those four steps transcribed literally, plus the construction-time normalisation the RFC
 * mandates. If the transcription and the RFC ever disagree, the RFC is what ships - fix this
 * file, not the spec, unless the spec is what is wrong.
 *
 * Build and run:
 *
 *   gcc -std=c2x -O2 -D_GNU_SOURCE -Iinclude -Iplatform \
 *       -o /tmp/rfc0003 docs/rfc-0003-spec-check.c src/proven/*.c platform/*.c && /tmp/rfc0003
 *
 * It checks two things:
 *   1. every row of RFC-0003 section 4.1 - the split behaviour table
 *   2. the two properties section 4.1 states: the iteration always terminates, and for a
 *      non-empty separator the field count equals non-overlapping left-to-right occurrences + 1
 *
 * Writing this is what found the hole. The first draft of section 3.1 omitted the
 * construction-time normalisation, so a {NULL,5} source produced a five-byte field over a NULL
 * pointer - eleven of twelve rows passed and the twelfth was a latent crash in the caller.
 * A specification nobody executes is a specification nobody has checked.
 */

#include "proven.h"
#include <stdio.h>
#include <string.h>

typedef struct { proven_u8str_view_t rest, sep; bool done; } split_t;

static proven_u8str_view_t norm(proven_u8str_view_t v){
    return (v.size > 0 && !v.ptr) ? (proven_u8str_view_t){0,0} : v;   /* ill-formed -> empty */
}
static split_t sp_begin(proven_u8str_view_t src, proven_u8str_view_t sep){
    return (split_t){ .rest=norm(src), .sep=norm(sep), .done=false }; /* normalise at construction */
}
/* steps 1-4, in the order the RFC mandates */
static bool sp_next(split_t *it, proven_u8str_view_t *out){
    if (!it || !out || it->done) return false;                                  /* 1 */
    if (it->sep.size == 0) { *out = it->rest; it->done = true; return true; }   /* 2 */
    proven_size_t at = proven_u8str_view_find(it->rest, 0, it->sep);
    if (at == PROVEN_INDEX_NOT_FOUND) { *out = it->rest; it->done = true; return true; } /* 3 */
    *out = proven_u8str_view_slice(it->rest, 0, at);                            /* 4 */
    it->rest = proven_u8str_view_slice(it->rest, at + it->sep.size,
                                       it->rest.size - at - it->sep.size);
    return true;
}

static void show(char *buf, proven_u8str_view_t v){
    size_t n = v.size < 20 ? v.size : 20;
    buf[0]='"'; memcpy(buf+1, v.ptr ? (const char*)v.ptr : "", n); buf[1+n]='"'; buf[2+n]=0;
}

static int check(const char*label, proven_u8str_view_t src, proven_u8str_view_t sep,
                 const char**expect, int nexp){
    split_t it = sp_begin(src, sep);
    proven_u8str_view_t f; int i=0, ok=1; char got[512]=""; char one[64];
    int guard = 0;
    while (sp_next(&it,&f)) {
        if (++guard > 50) { printf("%-22s NON-TERMINATING\n", label); return 1; }
        show(one,f); strcat(got,one); strcat(got," ");
        if (i>=nexp || (proven_size_t)strlen(expect[i]) != f.size ||
            (f.size && memcmp(expect[i],f.ptr,f.size))) ok=0;
        i++;
    }
    if (i!=nexp) ok=0;
    printf("%-22s got: %-28s %s\n", label, got, ok?"ok":"MISMATCH");
    return !ok;
}
#define V(s) proven_u8str_view_from_cstr(s)
static const proven_u8str_view_t NUL0 = {0,0};
static const proven_u8str_view_t NUL5 = {0,5};


/* independent oracle: count non-overlapping occurrences by naive scan */
static int occ(proven_u8str_view_t h, proven_u8str_view_t n){
    if(n.size==0||n.size>h.size) return 0;
    int c=0; proven_size_t i=0;
    while(i+n.size<=h.size){ if(!memcmp(h.ptr+i,n.ptr,n.size)){c++; i+=n.size;} else i++; }
    return c;
}

static int table_checks(void){
    int bad=0;
    { const char*e[]={"a","b","c"}; bad+=check("\"a,b,c\" / \",\"", V("a,b,c"), V(","), e,3); }
    { const char*e[]={"a"};         bad+=check("\"a\" / \",\"",     V("a"),     V(","), e,1); }
    { const char*e[]={"a",""};      bad+=check("\"a,\" / \",\"",    V("a,"),    V(","), e,2); }
    { const char*e[]={"","a"};      bad+=check("\",a\" / \",\"",    V(",a"),    V(","), e,2); }
    { const char*e[]={"a","","b"};  bad+=check("\"a,,b\" / \",\"",  V("a,,b"),  V(","), e,3); }
    { const char*e[]={""};          bad+=check("\"\" / \",\"",      V(""),      V(","), e,1); }
    { const char*e[]={""};          bad+=check("{NULL,0} / \",\"",  NUL0,       V(","), e,1); }
    { const char*e[]={"a","b"};     bad+=check("\"aXXb\" / \"XX\"", V("aXXb"),  V("XX"),e,2); }
    { const char*e[]={"a","Xb"};    bad+=check("\"aXXXb\"/\"XX\"",  V("aXXXb"), V("XX"),e,2); }
    { const char*e[]={"abc"};       bad+=check("\"abc\" / \"\"",    V("abc"),   V(""),  e,1); }
    { const char*e[]={"abc"};       bad+=check("\"abc\" / {NULL,0}",V("abc"),   NUL0,   e,1); }
    { const char*e[]={""};          bad+=check("{NULL,5} / \",\"",  NUL5,       V(","), e,1); }
    printf("\n%s\n", bad ? "SPEC DOES NOT SATISFY ITS OWN TABLE" : "spec satisfies every row of 4.1");
        return bad;
}

static int property_checks(void){

    proven_xoshiro256ss_t g; proven_xoshiro256ss_seed(&g,20260719);
    char hb[33], nb[5]; int fails=0, nonterm=0;
    for(long t=0;t<200000;t++){
        proven_u64 r=proven_xoshiro256ss_next(&g);
        int hl=(int)(r%17), nl=(int)((r>>8)%4);       /* needle length 0..3 exercises empty sep */
        for(int i=0;i<hl;i++) hb[i]="ab,"[proven_xoshiro256ss_next(&g)%3];
        for(int i=0;i<nl;i++) nb[i]="ab,"[proven_xoshiro256ss_next(&g)%3];
        proven_u8str_view_t h={(const proven_byte_t*)hb,(proven_size_t)hl};
        proven_u8str_view_t n={(const proven_byte_t*)nb,(proven_size_t)nl};
        split_t it=sp_begin(h,n); proven_u8str_view_t f; int cnt=0;
        while(sp_next(&it,&f)){ if(++cnt>200){nonterm++;break;} }
        if(cnt>200) continue;
        int expect = (nl==0) ? 1 : occ(h,n)+1;
        if(cnt!=expect){ if(fails<3) printf("FAIL hl=%d nl=%d got=%d want=%d\n",hl,nl,cnt,expect); fails++; }
    }
    printf("200000 random cases: %d count mismatches, %d non-terminating\n", fails, nonterm);
    printf("%s\n", (fails||nonterm) ? "PROPERTY VIOLATED" : "properties hold");
        return (fails||nonterm)!=0;
}

int main(void){
    printf("=== RFC-0003 section 4.1: behaviour table ===\n");
    int bad = table_checks();
    printf("\n=== RFC-0003 section 4.1: properties ===\n");
    bad += property_checks();
    printf("\n%s\n", bad ? "SPEC CHECK FAILED" : "spec check passed");
    return bad != 0;
}
