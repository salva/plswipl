#include "postgres.h"
#include "fmgr.h"
#include "funcapi.h"
#include "port.h"
#include "miscadmin.h"
#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/trigger.h"
#include "mb/pg_wchar.h"
#include "utils/palloc.h"
#include "utils/guc.h"
#include "utils/elog.h"
#include "utils/syscache.h"
#include "utils/builtins.h"

#include <SWI-Prolog.h>

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

PG_FUNCTION_INFO_V1(plswipl_handler);
PG_FUNCTION_INFO_V1(plswipl_inline);
PG_FUNCTION_INFO_V1(plswipl_validator);

PG_FUNCTION_INFO_V1(plswipl_function);

void _PG_init(void);

typedef struct {
    fid_t fid;
    qid_t qid;
    term_t a0;
    int nargs;
    char *argmodes;
    Oid *argtypes;
    Oid rettype;
    MemoryContextCallback callback;
} plswipl_swiplctx;

/* Global data */

static char *plswipl_argv[] = { "pl/swipl", "-p", "", "-f", "", NULL, };

void
_PG_init(void) {
    static bool inited = false;

    if (!inited) {
        char sharepath[MAXPGPATH];
	char *boot, *alias;
        int alias_size;
        char **p;

	get_share_path(my_exec_path, sharepath);

        alias_size = MAXPGPATH + 20;
        alias = (char *) palloc(alias_size);
        snprintf(alias, alias_size, "plswipl=%s/plswipl", sharepath);
        plswipl_argv[2] = alias;

	boot = (char *) palloc(MAXPGPATH);
	snprintf(boot, MAXPGPATH, "%s/plswipl/boot.prolog", sharepath);
        plswipl_argv[4] = boot;

        printf("swi-prolog args:");
        for (p = plswipl_argv; *p; p++) printf(" \"%s\"", *p);
        printf(".\n");

        if (PL_initialise(5, plswipl_argv)) {
            inited = 1;
            return;
        }
        PL_halt(1);
    }
}

Datum
plswipl_handler(PG_FUNCTION_ARGS) {
    Datum retval;

    if (CALLED_AS_TRIGGER(fcinfo)) {
        printf("function called as trigger!\n"); fflush(stdout);
        /*
         * Called as a trigger procedure
         */
        /* TriggerData    *trigdata = (TriggerData *) fcinfo->context; */
        exit(-1);
    }
    else {
        /*
         * Called as a function
         */

        retval = plswipl_function(fcinfo);
    }
    return retval;
}

static void
check_exception(qid_t qid, char *context) {
    term_t e = PL_exception(qid);
    if (e) {
        char *e_chars = NULL;
        if (PL_get_chars(e, &e_chars, CVT_ALL|CVT_VARIABLE|CVT_WRITE|BUF_RING|REP_UTF8))
            ereport(ERROR,
                    (errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
                     errmsg("exception %i %s", (int)e, (e_chars ? e_chars : "*UNKNOWN*")),
                     errcontext("%s", context)));
    }
}

Datum
plswipl_inline(PG_FUNCTION_ARGS) {
    InlineCodeBlock *codeblock = (InlineCodeBlock *) PG_GETARG_POINTER(0);
    static predicate_t predicate_handle_do = 0;
    fid_t fid;
    term_t a0;

    if (!predicate_handle_do)
        predicate_handle_do = PL_predicate("handle_do", 1, "plswipl_low");

    fid = PL_open_foreign_frame();
    a0 = PL_new_term_refs(1);
    if (PL_put_string_chars(a0, codeblock->source_text)) {
        qid_t qid = PL_open_query(NULL, PL_Q_CATCH_EXCEPTION, predicate_handle_do, a0);
        if (!PL_next_solution(qid))
            check_exception(qid, "while executing DO with PLSWIPL");
        PL_close_query(qid);
    }
    PL_discard_foreign_frame(fid);
    PG_RETURN_VOID();
}

Datum
plswipl_validator(PG_FUNCTION_ARGS) {
    PG_RETURN_VOID();
}

static int
cons_functor_chars(term_t out, const char *name, int arity, term_t args) {
    atom_t atom_name = PL_new_atom(name);
    int r = PL_cons_functor_v(out, PL_new_functor(atom_name, arity), args);
    PL_unregister_atom(atom_name);
    return r;
}

static char *
utf_e2u(const char *str) {
    printf("database encoding: %d [ascii: %d]\n", GetDatabaseEncoding(),PG_SQL_ASCII); fflush(stdout); 
    return pg_server_to_any(str, strlen(str), PG_UTF8);
}

static bool
plswipl_term_to_datum(term_t t, Oid type, Datum *datum) {
    switch(type) {
    case VOIDOID:
        *datum = (Datum)0;
        break;
    case BOOLOID: {
        int v;
        if (!PL_get_bool(t, &v)) return FALSE;
        *datum = BoolGetDatum(v);
        break;
    }
    case INT2OID:
    case INT4OID:
    case INT8OID: {
        int64_t v;
        if (!PL_get_int64(t, &v)) return FALSE;
        if (type == INT2OID) {
            if ((uint64_t)v >> 16) return FALSE;
            *datum = Int16GetDatum(v);
        }
        else if (type == INT4OID) {
            if ((uint64_t)v >> 32) return FALSE;
            *datum = Int32GetDatum(v);
        }
        else { /* INT8OID */
            *datum = Int64GetDatum(v);
        }
        break;
    }
    case TEXTOID: {
        char *v;
        if (!PL_get_chars(t, &v, CVT_ALL|BUF_RING|REP_UTF8)) return FALSE;
        *datum = PointerGetDatum(DirectFunctionCall1(textin, PointerGetDatum(v)));
        break;
    }
    default:
        return FALSE;
    }
    return TRUE;
}

static void
plswipl_clean_context(plswipl_swiplctx *swiplctx) {
    printf("plswipl_clean_context: %p, fid: %li, qid: %li\n",
           swiplctx, swiplctx->fid, swiplctx->qid); fflush(stdout);
    if (swiplctx->fid) {
        if (swiplctx->qid)
            PL_close_query(swiplctx->qid);
        PL_close_foreign_frame(swiplctx->fid);
    }
}

Datum
plswipl_function(PG_FUNCTION_ARGS) {
    Oid fn_oid = fcinfo->flinfo->fn_oid;
    ReturnSetInfo *rsi = (ReturnSetInfo *)fcinfo->resultinfo;
    HeapTuple procTup;
    Form_pg_proc procStruct;
    Datum proallargtypes, proargmodes, prosrcdatum;
    Oid *argtypes, rettype;
    char *argmodes;
    char *proSource;
    bool isNull, retvoid, ok;
    int nargs, i;
    fid_t fid;
    term_t a0, a1;
    qid_t qid;
    FuncCallContext  *funcctx = NULL;

    static predicate_t predicate_handle_function = 0;
    if (!predicate_handle_function)
        predicate_handle_function = PL_predicate("handle_function", 2, "plswipl_low");

    printf("fcinfo: %p, context: %p, resultinfo: %p, fncollation: %i, isnull: %d, nargs: %d\n",
           fcinfo, fcinfo->context, rsi, fcinfo->fncollation, fcinfo->isnull, fcinfo->nargs);
    if (rsi)
        printf("ReturnSetInfo: %p, type: %i, econtext: %p, expectedDesc: %p, allowedModes: %i, "
               "returnMode: %i, isDone: %i, setResult: %p, setDesc: %p\n",
               rsi, rsi->type, rsi->econtext, rsi->expectedDesc, rsi->allowedModes,
               rsi->returnMode, rsi->isDone, rsi->setResult, rsi->setDesc);
    fflush(stdout);

    if (SRF_IS_FIRSTCALL()) {
        plswipl_swiplctx *swiplctx;
        MemoryContext oldcontext = NULL;

        procTup = SearchSysCache1(PROCOID, ObjectIdGetDatum(fn_oid));
        if (!HeapTupleIsValid(procTup))
            elog(ERROR, "cache lookup failed for function %u", fn_oid);
        procStruct = (Form_pg_proc) GETSTRUCT(procTup);
        rettype = procStruct->prorettype;
        retvoid = (rettype == VOIDOID);
        nargs = procStruct->pronargs;

        proallargtypes = SysCacheGetAttr(PROCOID, procTup,  Anum_pg_proc_proallargtypes, &isNull);
        if (isNull) {
            if (procStruct->proargtypes.dim1 != nargs) elog(ERROR, "size mismatch between function arguments and type array");
            argtypes = procStruct->proargtypes.values;
        }
        else {
            ArrayType *arr = DatumGetArrayTypeP(proallargtypes);
            if ((ARR_NDIM(arr) != 1)       ||
                (ARR_DIMS(arr)[0] < nargs) ||
                ARR_HASNULL(arr)           ||
                (ARR_ELEMTYPE(arr) != OIDOID)) elog(ERROR, "proallargtypes is not a 1-D Oid array");
            nargs = ARR_DIMS(arr)[0];
            argtypes = (Oid*)ARR_DATA_PTR(arr);
        }

        proargmodes = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_proargmodes, &isNull);
        if (isNull)
            argmodes = NULL;
        else {
            ArrayType *arr = DatumGetArrayTypeP(proargmodes);
            if ((ARR_NDIM(arr) != 1 )       ||
                (ARR_DIMS(arr)[0] != nargs) ||
                ARR_HASNULL(arr)            ||
                (ARR_ELEMTYPE(arr) != CHAROID)) elog(ERROR, "proargmodes is not a 1-D char array");
            argmodes = (char*)ARR_DATA_PTR(arr);
        }

        if (procStruct->proretset) {
            funcctx = SRF_FIRSTCALL_INIT();
            oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
        }

        swiplctx =(plswipl_swiplctx*)palloc(sizeof(plswipl_swiplctx));
        swiplctx->fid = 0;
        swiplctx->qid = 0;
        swiplctx->callback.arg = (void *)swiplctx;
        swiplctx->callback.func = (MemoryContextCallbackFunction)plswipl_clean_context;
        MemoryContextRegisterResetCallback(CurrentMemoryContext, &(swiplctx->callback));

        fid = PL_open_foreign_frame();
        swiplctx->fid = fid;

        a0 = PL_new_term_refs(nargs + 1);

        for (i = 0; i < nargs; i++) {
            char argmode = (argmodes ? argmodes[i] : 'i');
            term_t a = a0 + i;
            switch (argmode) {
            case 'i':
                if (fcinfo->argnull[i])
                    PL_put_nil(a);
                else {
                    Datum datum = fcinfo->arg[i];
                    switch(argtypes[i]) {
                    case BOOLOID:
                        if (!PL_put_bool(a, DatumGetBool(datum))) goto error;
                        break;
                    case INT2OID:
                        if (!PL_put_integer(a, DatumGetInt16(datum))) goto error;
                        break;
                    case INT4OID:
                        if (!PL_put_integer(a, DatumGetInt32(datum))) goto error;
                        break;
                    case INT8OID:
                        if (!PL_put_int64(a, DatumGetInt64(datum))) goto error;
                        break;
                    case FLOAT4OID:
                        if (!PL_put_float(a, DatumGetFloat4(datum))) goto error;
                        break;
                    case FLOAT8OID:
                        if (!PL_put_float(a, DatumGetFloat8(datum))) goto error;
                        break;
                    case TEXTOID:
                        printf("converting Pg text '%s' to SWI-Prolog\n", TextDatumGetCString(datum)); fflush(stdout);
                        if (!PL_put_string_chars(a, utf_e2u(TextDatumGetCString(datum)))) goto error;
                        break;
                    default:
                    error:
                        ereport(ERROR,
                                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                 errmsg("PL/SWI-Prolog functions cannot accept type %s",
                                        format_type_be(argtypes[i]))));
                    }
                }
            case 'o':
                /* output: do nothing yet! */
                break;
            default:
                ereport(ERROR,
                        (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                         errmsg("PL/SWI-Prolog functions cannot accept argument mode '%c'",
                                argmode)));
            }
        }

        a1 = PL_new_term_refs(2);
        if (!cons_functor_chars(a1 + 0, procStruct->proname.data, (retvoid ? nargs : nargs + 1), a0)) {
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("PL/SWI-Prolog PL_cons_functor_v failed")));
        }

        prosrcdatum = SysCacheGetAttr(PROCOID, procTup, Anum_pg_proc_prosrc, &isNull);
        if (isNull)
            elog(ERROR, "null prosrc");
        proSource = TextDatumGetCString(prosrcdatum);
        if (!PL_put_string_chars(a1 + 1, proSource)) {
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("PL/SWI-Prolog PL_put_string_chars failed")));
        }

        qid = PL_open_query(NULL, PL_Q_CATCH_EXCEPTION, predicate_handle_function, a1);
        swiplctx->qid = qid;

        if (procStruct->proretset) {
            funcctx->user_fctx = swiplctx;
            swiplctx->a0 = a0;
            swiplctx->nargs = nargs;
            if (argmodes) {
                swiplctx->argmodes = pnstrdup(argmodes, nargs);
                swiplctx->argtypes = palloc(nargs * sizeof(Oid));
                memcpy(swiplctx->argtypes, argtypes, nargs * sizeof(Oid));
            }
            else {
                swiplctx->argmodes = NULL;
                swiplctx->argtypes = NULL;
            }
            swiplctx->rettype = rettype;
            MemoryContextSwitchTo(oldcontext);
        }

        ReleaseSysCache(procTup);
    }
    else {
        plswipl_swiplctx *swiplctx;
        funcctx = SRF_PERCALL_SETUP();
        swiplctx = (plswipl_swiplctx*)funcctx->user_fctx;
        fid = swiplctx->fid;
        qid = swiplctx->qid;
        a0 = swiplctx->a0;
        nargs = swiplctx->nargs;
        argmodes = swiplctx->argmodes;
        argtypes = swiplctx->argtypes;
        rettype = swiplctx->rettype;

        printf("SRF again!\n");
    }

    printf("fid: %li, qid: %li, a0: %li, nargs: %i, argmodes: %s, argtypes: %p, rettype: %i\n",
           fid, qid, a0, nargs, argmodes, argtypes, rettype); fflush(stdout);

    ok = PL_next_solution(qid);
    if (ok) {
        Datum out;
        if (argmodes) {
            for (i = 0; i < nargs; i++) {
                if (argmodes[i] == 'o') {
                    if (!plswipl_term_to_datum(a0 + i, argtypes[i], fcinfo->arg + i))
                        ereport(ERROR,
                                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                                 errmsg("PL/SWI-Prolog cannot convert output argument %i to type %s",
                                        i, format_type_be(argtypes[i]))));
                }
            }
        }
        if (!plswipl_term_to_datum(a0 + nargs, rettype, &out))
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("PL/SWI-Prolog cannot convert output value to type %s",
                            format_type_be(rettype))));

        if (funcctx)
            SRF_RETURN_NEXT(funcctx, out);

        return out;
    }

    check_exception(qid, "while callin PLSWIPL function");
    if (funcctx)
        SRF_RETURN_DONE(funcctx);
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("PL/SWI-Prolog function failed")));
}

