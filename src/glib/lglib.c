/* lglib.c - Lua binding of GLib Regex library */
/* See Copyright Notice in the file LICENSE */

/* This is similar to Lrexlib's PCRE implementation, but has been changed
 *   for GLib's pcre implementation, which is different.
 * Changes:
 *  - Removed chartables and locale handling
 *  - dfa_exec doesn't take 'ovecsize' nor 'wscount' args
 *  - dfa_exec returns boolean true for partial match, without subcapture info
 *  - named subgroups do not return a table of name-keyed entries, because
 *    GLib doesn't provide a way to learn that information
 *  - there is no 'config()' function, since GLib doesn't offer such info
 *  - the 'flags()' function still works, returning all flags, but two new
 *    functions 'compile_flags()' and 'match_flags()' return just their respective
 *    flags, since GLib has a different and smaller set of such flags, for
 *    regex compile vs. match functions
 *  - obviously quite a bit else is changed to interface to GLib's regex instead
 *    of PCRE, but hopefully those changes aren't visible to user/caller
 */

#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <ctype.h>
#include <glib.h>

#include "lua.h"
#include "lauxlib.h"
#include "common.h"

extern int Gregex_get_flags (lua_State *L);
extern int Gregex_get_compile_flags (lua_State *L);
extern int Gregex_get_match_flags (lua_State *L);
extern flag_pair gregex_error_flags[];

/* These 2 settings may be redefined from the command-line or the makefile.
 * They should be kept in sync between themselves and with the target name.
 */
#ifndef REX_LIBNAME
# ifdef WIRESHARK
#  define REX_LIBNAME "GRegex"
# else
#  define REX_LIBNAME "rex_glib"
# endif
#endif
#ifndef REX_OPENLIB
#  define REX_OPENLIB luaopen_rex_glib
#endif

#define REX_TYPENAME REX_LIBNAME"_regex"

#define ALG_CFLAGS_DFLT 0
#define ALG_EFLAGS_DFLT 0

static int getcflags (lua_State *L, int pos);
#define ALG_GETCFLAGS(L,pos)  getcflags(L, pos)

#define ALG_NOMATCH(res)   ((res) == FALSE)
#define ALG_ISMATCH(res)   ((res) == TRUE)
#define ALG_SUBBEG(ud,n)   getSubStartPos(ud,n)
#define ALG_SUBEND(ud,n)   getSubEndPos(ud,n)
#define ALG_SUBLEN(ud,n)   (ALG_SUBEND(ud,n) - ALG_SUBBEG(ud,n))
#define ALG_SUBVALID(ud,n) (ALG_SUBBEG(ud,n) >= 0)
#define ALG_NSUB(ud)       ((int) g_regex_get_capture_count(ud->pr))

#define ALG_PUSHSUB(L,ud,text,n) \
  lua_pushlstring (L, (text) + ALG_SUBBEG(ud,n), ALG_SUBLEN(ud,n))

#define ALG_PUSHSUB_OR_FALSE(L,ud,text,n) \
  (ALG_SUBVALID(ud,n) ? ALG_PUSHSUB (L,ud,text,n) : lua_pushboolean (L,0))

#define ALG_PUSHSTART(L,ud,offs,n)   lua_pushinteger(L, (offs) + ALG_SUBBEG(ud,n) + 1)
#define ALG_PUSHEND(L,ud,offs,n)     lua_pushinteger(L, (offs) + ALG_SUBEND(ud,n))
#define ALG_PUSHOFFSETS(L,ud,offs,n) \
  (ALG_PUSHSTART(L,ud,offs,n), ALG_PUSHEND(L,ud,offs,n))

#define ALG_BASE(st)  0
#define ALG_PULL
/* we define ALG_USERETRY because Glib does expose PCRE's NOTEMPTY and ANCHORED flags */
#define ALG_USERETRY

typedef struct {
  GRegex     * pr;
  GMatchInfo * match_info;
  GError     * error; /* didn't want to put this here, but can't free it otherwise */
  int          freed;
} TGrgx;

static void minfo_free(TGrgx* ud) {
  if (ud->match_info)
    g_match_info_free (ud->match_info);
  ud->match_info = NULL;
}

static void gerror_free(TGrgx* ud) {
  if (ud->error)
    g_error_free (ud->error);
  ud->error = NULL;
}

static int getSubStartPos(TGrgx* ud, int n) {
  int start_pos = -1;
  g_match_info_fetch_pos (ud->match_info, n, &start_pos, NULL);
  return start_pos;
}

static int getSubEndPos(TGrgx* ud, int n) {
  int end_pos = -1;
  g_match_info_fetch_pos (ud->match_info, n, NULL, &end_pos);
  return end_pos;
}

#define TUserdata TGrgx

/* TODO: handle named subpatterns somehow */
#if 0
static void do_named_subpatterns (lua_State *L, TGrgx *ud, const char *text);
#  define DO_NAMED_SUBPATTERNS do_named_subpatterns
#endif

#include "lrexlib_algo.h"

/*  Functions
 ******************************************************************************
 */

static int getcflags (lua_State *L, int pos) {
  switch (lua_type (L, pos)) {
    case LUA_TNONE:
    case LUA_TNIL:
      return ALG_CFLAGS_DFLT;
    case LUA_TNUMBER:
      return (int) lua_tointeger (L, pos);
    case LUA_TSTRING: {
      const char *s = lua_tostring (L, pos);
      GRegexCompileFlags res = 0, ch;
      while ((ch = *s++) != '\0') {
        if (ch == 'i') res |= G_REGEX_CASELESS;
        else if (ch == 'm') res |= G_REGEX_MULTILINE;
        else if (ch == 's') res |= G_REGEX_DOTALL;
        else if (ch == 'x') res |= G_REGEX_EXTENDED;
        else if (ch == 'U') res |= G_REGEX_UNGREEDY;
      }
      return (int)res;
    }
    default:
      return luaL_typerror (L, pos, "number or string");
  }
}

/* this function is used in algo.h as well */
static int generate_error (lua_State *L, const TGrgx *ud, int errcode) {
  const char *key = get_flag_key (gregex_error_flags, ud->error->code);
  (void) errcode;
  if (key)
    return luaL_error (L, "error G_REGEX_%s (%s)", key, ud->error->message);
  else
    return luaL_error (L, "Glib Regex error: %s (code %d)", ud->error->message, ud->error->code);
}


static int compile_regex (lua_State *L, const TArgComp *argC, TGrgx **pud) {
  TGrgx *ud;

  ud = (TGrgx*)lua_newuserdata (L, sizeof (TGrgx));
  memset (ud, 0, sizeof (TGrgx));           /* initialize all members to 0 */
  lua_pushvalue (L, ALG_ENVIRONINDEX);
  lua_setmetatable (L, -2);

  ud->pr = g_regex_new (argC->pattern, argC->cflags, 0, &ud->error);
  if (!ud->pr)
    return luaL_error (L, "%s (code: %d)", ud->error->message, ud->error->code);

  if (pud) *pud = ud;
  return 1;
}

/* unfortunately glib doesn't provide a way to learn what names are in the
   pattern - it supports named groups, but expects the function caller to already
   know what names to get.
 */
#if 0
/* the target table must be on lua stack top */
static void do_named_subpatterns (lua_State *L, TGrgx *ud, const char *text) {
  int i, namecount, name_entry_size;
  unsigned char *name_table, *tabptr;

  /* do named subpatterns - NJG */
  pcre_fullinfo (ud->pr, ud->extra, PCRE_INFO_NAMECOUNT, &namecount);
  if (namecount <= 0)
    return;
  pcre_fullinfo (ud->pr, ud->extra, PCRE_INFO_NAMETABLE, &name_table);
  pcre_fullinfo (ud->pr, ud->extra, PCRE_INFO_NAMEENTRYSIZE, &name_entry_size);
  tabptr = name_table;
  for (i = 0; i < namecount; i++) {
    int n = (tabptr[0] << 8) | tabptr[1]; /* number of the capturing parenthesis */
    if (n > 0 && n <= ALG_NSUB(ud)) {   /* check range */
      lua_pushstring (L, (char *)tabptr + 2); /* name of the capture, zero terminated */
      ALG_PUSHSUB_OR_FALSE (L, ud, text, n);
      lua_rawset (L, -3);
    }
    tabptr += name_entry_size;
  }
}
#endif

/* method r:dfa_exec (s, [st], [ef]) */
static void checkarg_dfa_exec (lua_State *L, TArgExec *argE, TGrgx **ud) {
  *ud = check_ud (L);
  argE->text = luaL_checklstring (L, 2, &argE->textlen);
  argE->startoffset = get_startoffset (L, 3, argE->textlen);
  argE->eflags = luaL_optint (L, 4, ALG_EFLAGS_DFLT);
}

/* unlike PCRE, partial matching won't return the actual substrings/matches */
static int Gregex_dfa_exec (lua_State *L)
{
  TArgExec argE;
  TGrgx *ud;
  gboolean res;

  checkarg_dfa_exec (L, &argE, &ud);

  gerror_free(ud);

  res = g_regex_match_all_full (ud->pr, argE.text, (int)argE.textlen,
    argE.startoffset, argE.eflags, &ud->match_info, &ud->error);

  if (ALG_ISMATCH (res)) {
    int i, start_pos, end_pos;
    int max = g_match_info_get_match_count (ud->match_info);
    g_match_info_fetch_pos (ud->match_info, 0, &start_pos, NULL);
    lua_pushinteger (L, start_pos + 1);         /* 1-st return value */
    lua_newtable (L);                            /* 2-nd return value */
    for (i=0; i<max; i++) {
      g_match_info_fetch_pos (ud->match_info, i, &start_pos, &end_pos);
      /* I don't know why these offsets aren't incremented by 1 to match Lua indexing? */
      lua_pushinteger (L, start_pos);
      lua_rawseti (L, -2, i+i+1);
      lua_pushinteger (L, end_pos);
      lua_rawseti (L, -2, i+i+2);
    }
    lua_pushinteger (L, max);                    /* 3-rd return value */
    minfo_free (ud);
    return 3;
  }
  else if (g_match_info_is_partial_match(ud->match_info)) {
    lua_pushboolean(L,1);
    minfo_free (ud);
    return 1;
  }
  else {
    minfo_free (ud);
    if (ALG_NOMATCH (res))
      return lua_pushnil (L), 1;
    else
      return generate_error (L, ud, 0);
  }
}

#ifdef ALG_USERETRY
  static int gmatch_exec (TUserdata *ud, TArgExec *argE, int retry) {
    minfo_free (ud);
    gerror_free(ud);
    int eflags = retry ? (argE->eflags|G_REGEX_MATCH_NOTEMPTY|G_REGEX_MATCH_ANCHORED) : argE->eflags;
    return g_regex_match_full (ud->pr, argE->text, argE->textlen,
      argE->startoffset, eflags, &ud->match_info, &ud->error);
  }
#else
  static int gmatch_exec (TUserdata *ud, TArgExec *argE) {
    minfo_free (ud);
    gerror_free(ud);
    return g_regex_match_full (ud->pr, argE->text, argE->textlen,
      argE->startoffset, argE->eflags, &ud->match_info, &ud->error);
  }
#endif

static void gmatch_pushsubject (lua_State *L, TArgExec *argE) {
  lua_pushlstring (L, argE->text, argE->textlen);
}

static int findmatch_exec (TGrgx *ud, TArgExec *argE) {
  minfo_free (ud);
  gerror_free(ud);
  return g_regex_match_full (ud->pr, argE->text, argE->textlen,
    argE->startoffset, argE->eflags, &ud->match_info, &ud->error);
}

#ifdef ALG_USERETRY
  static int gsub_exec (TGrgx *ud, TArgExec *argE, int st, int retry) {
    minfo_free (ud);
    gerror_free(ud);
    int eflags = retry ? (argE->eflags|G_REGEX_MATCH_NOTEMPTY|G_REGEX_MATCH_ANCHORED) : argE->eflags;
    return g_regex_match_full (ud->pr, argE->text, argE->textlen,
      st, eflags, &ud->match_info, &ud->error);
  }
#else
  static int gsub_exec (TGrgx *ud, TArgExec *argE, int st) {
    minfo_free (ud);
    gerror_free(ud);
    return g_regex_match_full (ud->pr, argE->text, argE->textlen,
      st, argE->eflags, &ud->match_info, &ud->error);
  }
#endif

static int split_exec (TGrgx *ud, TArgExec *argE, int offset) {
  minfo_free (ud);
  gerror_free(ud);
  return g_regex_match_full (ud->pr, argE->text, argE->textlen, offset,
                    argE->eflags, &ud->match_info, &ud->error);
}

static int Gregex_gc (lua_State *L) {
  TGrgx *ud = check_ud (L);
  if (ud->freed == 0) {           /* precaution against "manual" __gc calling */
    ud->freed = 1;
    if (ud->pr) g_regex_unref (ud->pr);
    minfo_free (ud);
    gerror_free (ud);
  }
  return 0;
}

static int Gregex_tostring (lua_State *L) {
  TGrgx *ud = check_ud (L);
  if (ud->freed == 0)
    lua_pushfstring (L, "%s (%p)", REX_TYPENAME, (void*)ud);
  else
    lua_pushfstring (L, "%s (deleted)", REX_TYPENAME);
  return 1;
}

static int Gregex_version (lua_State *L) {
  lua_pushfstring (L, "%d.%d.%d", GLIB_MAJOR_VERSION, GLIB_MINOR_VERSION, GLIB_MICRO_VERSION);
  return 1;
}


static const luaL_Reg r_methods[] = {
  { "exec",        algm_exec },
  { "tfind",       algm_tfind },    /* old name: match */
  { "find",        algm_find },
  { "match",       algm_match },
  { "dfa_exec",    Gregex_dfa_exec },
  { "__gc",        Gregex_gc },
  { "__tostring",  Gregex_tostring },
  { NULL, NULL }
};

static const luaL_Reg r_functions[] = {
  { "match",       algf_match },
  { "find",        algf_find },
  { "gmatch",      algf_gmatch },
  { "gsub",        algf_gsub },
  { "split",       algf_split },
  { "new",         algf_new },
  { "flags",       Gregex_get_flags },
  { "compile_flags", Gregex_get_compile_flags },
  { "match_flags", Gregex_get_match_flags },
  { "version",     Gregex_version },
  { NULL, NULL }
};

/* Open the library */
REX_API int REX_OPENLIB (lua_State *L) {

  alg_register(L, r_methods, r_functions, "GLib Regex");

  return 1;
}