/* $Id$ */

/*
 * Copyright 2011, The TenDRA Project.
 * Copyright 1997, United Kingdom Secretary of State for Defence.
 *
 * See doc/copyright/ for the full copyright terms.
 */

/*
 * Translation is controlled by translate () in this module.
 * Code generation follows the following phases :
 *
 * 1. The TDF is read in, applying bottom-up optimisations.
 * 2. Top-down optimisations are performed.
 * 3. Register allocation is performed, and TDF idents introduced
 * so code generation can be performed with no register spills.
 * 4. Code is generated for each procedure, and global declarations processed.
 * 5. Currently assembler source is generated directly, and the
 * assembler optimiser ( as -O ) used for delay slot filling,
 * instruction scheduling and peep-hole optimisation.
 *
 * In a little more detail :
 *
 * 1 ) During the TDF reading process for tag declarations and tag
 * definitions, applications of tokens are expanded as they are
 * encountered, using the token definitions. Every token used must have
 * been previously defined in the bitstream.
 *
 * The reading of the tag definitions creates a data structure in memory
 * which directly represents the TDF. At present, all the tag definitions
 * are read into memory in this way before any further translation is
 * performed. This will shortly be changed, so that translation is
 * performed in smaller units. The translator is set up already so that
 * the change to translate in units of single global definitions ( or
 * groups of these ) can easily be made.
 *
 * During the creation of the data structure bottom-up optimisations
 * are performed. These are the optimisations which can be done when a
 * complete sub-tree of TDF is present, and are independent of the context
 * in which the sub-tree is used. They are defined in refactor.c and
 * refactor_id.c. These optimisation do such things as use the commutative
 * and associative laws for plus to collect together and evaluate
 * constants. More ambitious examples of these bottom-up optimisations
 * include all constant evaluation, elimination of inaccessible code, and
 * forward propagation of assignments of constants.
 *
 * 2 ) After reading in the TDF various optimisations are performed which
 * cannot be done until the whole context is present. For example,
 * constants cannot be extracted from a loop when we just have the loop
 * itself, because we cannot tell whether the variables used in it are
 * aliased.
 *
 * These optimisations are invoked by opt_all_exps which is defined in
 * indep.c. They include extraction of constants from loops, common
 * expression elimination on them and strength reduction for indexing.
 *
 * 3 ) Allocatable registers are partitioned into two sets, the s regs
 * which are preserved over calls, and the t regs which are not.
 *
 * The TDF is scanned introducing TDF idents so that expressions can be
 * evaluated within the available t regs with no spills. These new idents
 * may be later allocated to a s reg later, if the weighting algorithm
 * ( below ) considers this worth while. Otherwise they will be on the stack.
 *
 * Information is collected to help in global register allocation. During
 * a forward recursive scan of the TDF the number of accesses to each
 * variable is computed ( making assumptions about the frequency of
 * execution of loops ) . Then on the return scan of this recursion, for
 * each declaration, the number of registers which must be free for it to
 * be worthwhile to use a register is computed, and put into the TDF as
 * the "break" point. The procedures to do this are defined in weights.c.
 *
 * Suitable idents not alive over a procedure call are allocated to a t reg,
 * and others to s regs. At the same time stack space requirements are
 * calculated, so this is known before code for a procedure is generated.
 *
 * 4 ) Finally the code is generated without register spills. The code is
 * generated by make_code () in make_code.c, and make_XXX_code () in proc.c.
 *
 * Note that procedure inlining and loop unrolling optimisations are not
 * currently implemented. Library procedures such as memcpy () and
 * strcpy () are not treated specially. Integer multiply, divide and
 * remainder use the standard support procedures .mul, .div, .rem and
 * unsigned variants.
 */

#include <assert.h>
#include <string.h>
#include <stdio.h>

#include <shared/error.h>
#include <shared/xalloc.h>

#include <local/ash.h>
#include <local/out.h>

#ifdef DWARF2
#include <local/dw2_config.h>
#endif

#include <reader/exp.h>
#include <reader/token.h>
#include <reader/basicread.h>
#include <reader/externs.h>

#include <construct/flpt.h>
#include <construct/tags.h>
#include <construct/exp.h>
#include <construct/shape.h>
#include <construct/tags.h>
#include <construct/installglob.h>

#include <refactor/optimise.h>

#include <main/driver.h>
#include <main/flags.h>

#include "tempdecs.h"
#include "weights.h"
#include "proctypes.h"
#include "procrec.h"
#include "regalloc.h"
#include "code_here.h"
#include "make_code.h"
#include "eval.h"
#include "bitsmacs.h"
#include "scan.h"
#include "getregs.h"
#include "regmacs.h"
#include "labels.h"
#include "comment.h"
#include "translate.h"
#include "proc.h"
#include "locate.h"
#include "sparctrans.h"
#include "localexpmacs.h"

#ifdef TDF_DIAG4
#include "stabs_diag4.h"
#else
#include "stabs_diag3.h"
#endif

#ifdef DWARF2
#include <dwarf2/dw2_iface.h>
#endif

extern bool know_size ;

char * proc_name;


#define isINITproc(id) (!strncmp("___I.TDF",id,8))



/*
  VARIABLES
*/

int sysV_assembler ;	/* System V version? */
int optim_level ;	/* optimisation level */
int g_reg_max ;		/* the maximum number of G registers */
int maxfix_tregs ;	/* the maximum number of T registers */
dec **main_globals ;	/* global declarations */
procrec *procrecs ;	/* procedure records */
dec *diag_def = NULL ;	/* diagnostics definition */
int main_globals_index = 0;	/* number of globals */
int caller_offset_used = 0;
enum section current_section = no_section ;


/*
  IS A SHAPE OF KNOWN SIZE?
*/

#define varsize( s )	1


/*
  OUTPUT A SECTION DIRECTIVE
*/

void 
insection ( enum section s ){
  if (!sysV_assembler && s == rodata_section)
    s = data_section;
  if ( s == current_section ) return ;
  current_section = s ;
  if ( sysV_assembler ) {
    switch ( s ) {
      case data_section :
	if (do_prom)
	  error(ERROR_INTERNAL, "prom .data");
	outs ( "\t.section\t\".data\"\n" ) ;
	return ;
      case text_section :
	outs ( "\t.section\t\".text\"\n" ) ;
	return ;
      case rodata_section :
	outs ( "\t.section\t\".rodata\"\n" ) ;
	return ;
      case init_section :
	outs ("\t.section\t\".init\"\n");
	return;
      default :
	break ;
    } 
  }
  else {
    switch ( s ) {
      case data_section :
	outs ( "\t.seg\t\"data\"\n" ) ;
	return ;
      case text_section :
	outs ( "\t.seg\t\"text\"\n" ) ;
	return ;
      default :
	break ;
    }
  }
  current_section = no_section ;
  error(ERROR_SERIOUS,  "bad \".section\" name" ) ;
  return ;
}	


/*
  MARK AN EXPRESSION AS UNALIASED
*/

void 
mark_unaliased ( exp e ){
  exp p = pt ( e ) ;
  bool ca = 1 ;
  assert ( !separate_units ) ;
  while ( p != NULL && ca ) {
    exp q = bro ( p ) ;
    if ( q == NULL ) {
#ifdef TDF_DIAG4
      if (!isdiaginfo(p))
#endif
        ca = 0 ;
    } 
    else {
      if ( !( last ( p ) && name ( q ) == cont_tag ) &&
	   !( !last ( p ) && last ( q ) &&
	      name ( bro ( q ) ) == ass_tag ) ) {
#ifdef TDF_DIAG4
        if (!isdiaginfo(p))
#endif
	  ca = 0 ;
      }
    }
    p = pt ( p ) ;
  }
  if ( ca ) setcaonly ( e ) ;
  return ;
}	


/*
  INITIALISE TRANSLATOR
*/
static void
init_translator (void)
{
  /* initialise nowhere */
  setregalt ( nowhere.answhere, 0 ) ;
  nowhere.ashwhere.ashsize = 0 ;
  nowhere.ashwhere.ashsize = 0 ;
  
  /* mark the as output as TDF compiled */
  outs ( lab_prefix ) ;
  outs ( "TDF.translated:\n" ) ;
  outs ( "!\tTDF->SPARC " ) ;
  if ( sysV_assembler ) outs ( " (SysV)" ) ;
  outnl () ;


  /* start diagnostics if necessary */
#ifdef DWARF2
  if (diag == DIAG_DWARF2)
    init_dwarf2 ();
  else
#endif
  if (diag != DIAG_NONE) {
#if DWARF1
    out_diagnose_prelude () ;
#else
#ifdef STABS
    init_stab () ;
#endif
#endif
  }
    /* start in text section */
  insection ( text_section ) ;
  return ;
}


/*
  Return the tag with name 'tag_name'
*/
baseoff 
find_tag ( char * tag_name ){
  int i;
  for (i=0; i<main_globals_index; ++i){
    exp newtag = main_globals[i]->dec_u.dec_val.dec_exp;
    char * id = main_globals[i]->dec_u.dec_val.dec_id;
    if(!strcmp(id,tag_name)) return boff(newtag);
  }
  printf("%s\n: ",tag_name);
  error(ERROR_SERIOUS, "tag not declared");
  exit(1);
}


/*
  EXIT TRANSLATOR
*/
void 
exit_translator (void){
  output_special_routines () ;
  insection ( text_section ) ;

#ifdef DWARF2
  if (diag == DIAG_DWARF2)
    end_dwarf2 ();
  else
#endif
  if (diag != DIAG_NONE) {
#if DWARF1
    out_diagnose_postlude () ;
#else
    /* do nothing */
#endif
  }
  return ;
}


/*
  TRANSLATE AN ENTIRE TDF CAPSULE
*/
void 
translate_capsule (void){
  int i ;
  int r ;
  dec *d ;
  int procno ;
  int noprocs ;
  space tempregs ;
  
  /* initialize diagnostics */
#ifdef DWARF2
  if ( diag != DIAG_NONE && diag != DIAG_DWARF2 ) {
#else
  if ( diag != DIAG_NONE ) {
#endif
#if DWARF1
    /* do nothing */
#else
#ifdef STABS
    init_stab_aux () ;
#endif
#endif
  }


  /* apply all optimisations */
#ifdef TDF_DIAG4
  if ( !diag_visible ) opt_all_exps () ;
#else
  if ( diag == DIAG_NONE ) opt_all_exps () ;
#endif
  
  /* mark all statics as unaliased and count procedures */
  noprocs = 0 ;
  for ( d = top_def ; d != NULL ; d = d->def_next ) {
    exp c = d->dec_u.dec_val.dec_exp ;
    if ( son ( c ) != NULL ) {
#ifdef TDF_DIAG4
      if ( !diag_visible && !separate_units && !d->dec_u.dec_val.extnamed
#else
      if ( diag == DIAG_NONE && !separate_units && !d->dec_u.dec_val.extnamed
#endif
	   && isvar ( c ) ) {
	mark_unaliased ( c ) ;
      }
      if ( name ( son ( c ) ) == proc_tag || 
	   name(son(c))==general_proc_tag) noprocs++ ;
    }	
  }	


	init_translator () ;

#ifdef DWARF2
  if (diag == DIAG_DWARF2) {
		/* Dump abbreviations table */
		do_abbreviations () ;
	    if ( dump_abbrev ) {
		  dwarf2_prelude () ;
	    }
		make_dwarf_common () ;
		dwarf2_postlude () ;
		exit_translator () ;
		exit(EXIT_FAILURE);
  }
#endif


  /* output weak symbols */
  while ( weak_list ) {
    outs ( "\t" ) ;
    outs ( weak_list->weak_id ) ;
    outs ( "=" ) ;
    outs ( weak_list->val_id ) ;
    outnl () ;
    weak_list = weak_list->next ;
  }
  
  /* allocate memory for procedures */
  if (noprocs == 0) {
    procrecs = NULL;
  } else {
    procrecs = ( procrec * ) xcalloc ( noprocs, sizeof ( procrec ) ) ;
  }
  
  /* number procedure definitions */
  procno = 0 ;
  for ( d = top_def ; d != NULL ; d = d->def_next ) {
    exp c = d->dec_u.dec_val.dec_exp ;
    exp s = son ( c ) ;
    if ( s != NULL && (name ( s ) == proc_tag || 
			 name(s) == general_proc_tag)) {
      procrec *pr = &procrecs [ procno ] ;
      pr->nameproc = bro ( c ) ;
      no ( s ) = procno++ ;
    }
  }

    /*
      Scan to put everything in SPARC form, and calculate register 
      and stack space needs.  First work out which t fixed point 
      registers	(those not preserved over calls) can be used.  This 
      needs to be done before scan () which adds identities so that 
      temporary	register needs are within the available temporary 
      register set.  We dynamically calculate g registers we can use
      as temporaries so	that we can support :
	    SUNOS 4 : %g1..%g7 can be used,
	    SYSV SPARC ABI : %g1..%g4 can be used.
    */

    /* initialise register sets */
  tempregs.fixed = PROC_TREGS ;
  tempregs.flt = PROC_FLT_TREGS ;

  /*permit use of valid g registers(%g0 is not a proper register)*/

  /* ensure R_TMP not allocatable */

  /* scan all the procedures, to put everything in SPARC operand form */
  for ( d = top_def ; d != NULL ; d = d->def_next ) {
    exp c = d->dec_u.dec_val.dec_exp ;
    exp s = son ( c ) ;
    if ( s != NULL && (name ( s ) == proc_tag ||
			 name(s) == general_proc_tag)) {
      exp *st = &s ;
      procrec *pr = &procrecs [ no ( s ) ] ;

      if (dyn_init && d->dec_u.dec_val.extnamed && isINITproc(d->dec_u.dec_val.dec_id))
	set_proc_uses_external (s);    /* for PIC_code, should be done in install_fns? */

      Has_vcallees = (name(s) == general_proc_tag) && 
	proc_has_vcallees(s);
      for (r=R_G1; r <= R_G0 + g_reg_max ; r++ ) {
	tempregs.fixed &= ~RMASK ( r ) ;
      }
      tempregs.fixed |= RMASK ( R_TMP ) ;
      /* count the number of fixed t registers */
      maxfix_tregs = 0 ;
      for ( r = R_FIRST ; r <= R_LAST ; r++ ) {
	if ( IS_TREG ( r ) && ( tempregs.fixed & RMASK ( r ) ) == 0 ) {
	  maxfix_tregs++ ;
	}
      }
      if (name(s) == general_proc_tag) {
	int any_envoff = 0;
	exp a = son (s);
	while (name(a) == ident_tag && isparam(a) && name(son(a)) != formal_callee_tag) {
	  if (isenvoff(a) && caller_offset_used)
	    any_envoff = 1;
	  else if (any_envoff) {
	    setvis(a);
	    setenvoff(a);
	  }
	  a = bro(son(a));
	}
	if (gencompat && ((name(a) == ident_tag && isparam(a)) || Has_vcallees)) {
	  set_proc_may_have_callees(s);
	}
      }	
      pr->needsproc = scan ( st, &st ) ;
      pr->needsproc.callee_size = (callee_size+63)&~63;
    }
  }

#if 0
    /* apply dead variable analysis (not currently implemented) */
  if ( do_deadvars ) {
    init_dead () ;
    dead_flag = 0 ;
    for ( d = top_def ; d != NULL ; d = d->def_next ) {
      exp c = d->dec_u.dec_val.dec_exp ;
      exp s = son ( c ) ;
      if ( s != NULL && (name ( s ) == proc_tag || 
			   name(s) == general_proc_tag)){
	deadvar ( s ) ;
      }
    }
  }
#endif


  /* calculate the break points for register allocation */
  for ( d = top_def ; d != NULL ; d = d->def_next ) {
    exp c = d->dec_u.dec_val.dec_exp ;
    exp s = son ( c ) ;
    if ( s != NULL && (name ( s ) == proc_tag || 
			 name(s) == general_proc_tag)) {
      weights w ;	
      spacereq forrest ;
      int freefixed, freefloat ;
      procrec *pr = &procrecs [ no ( s ) ] ;
      
      avoid_L7 = ( bool )( PIC_code && proc_uses_external ( s ) ) ;
      Has_vcallees = (name(s) == general_proc_tag) && 
	proc_has_vcallees(s);
      in_general_proc = (name(s) == general_proc_tag);
      if (gencompat) {
        May_have_callees = proc_may_have_callees(s);
      }
      /* calculate number of free registers */
      freefixed = ( R_L7 - R_L0 + 1 ) + ( R_I5 - R_I0 + 1 ) ;
      freefloat = 0 ;
      if ( avoid_L7 ) freefixed-- ;
      if (gencompat) {
        if (May_have_callees) freefixed--;
      } else {
        if(in_general_proc) freefixed--;
      }
      if(Has_vcallees) freefixed --;
      /* estimate tag usage */
      w = weightsv ( 1.0, bro ( son ( s ) ) ) ;
      /* calculate register and stack allocation for tags */
      forrest = regalloc ( bro ( son ( s ) ), freefixed, freefloat, 0 ) ;
      pr->spacereqproc = forrest ;
    }
  }
  /* set up main_globals */
  i = 0 ;
  for ( d = top_def ; d != NULL ; d = d->def_next ) i++ ;
  main_globals_index = i;
  if ( i ) main_globals = ( dec ** ) xcalloc ( i, sizeof ( dec * ) ) ;
  i = 0 ;
  for ( d = top_def ; d != NULL ; d = d->def_next ) {
    main_globals [i] = d ;
    main_globals [i]->dec_u.dec_val.sym_number = i ;
    i++ ;
  }
  /* output global definitions */
  for ( d = top_def ; d != NULL ; d = d->def_next ) {
    exp tg = d->dec_u.dec_val.dec_exp ;
    exp stg = son ( tg ) ;
    char *id = d->dec_u.dec_val.dec_id ;
    bool extnamed = ( bool ) d->dec_u.dec_val.extnamed ;
    if ( stg != NULL && (extnamed || no(tg)!= 0 ||
	 !strcmp(id,TDF_HANDLER) || !strcmp(id,TDF_STACKLIM))) {
      if ( extnamed ) {
	assert ( id [ 0 ] != '$' ) ;
	if ( name ( stg ) != proc_tag && name(stg)!=general_proc_tag) {
	  if (!isvar (tg) || (d -> dec_u.dec_val.acc & f_constant) || do_prom) 
	    insection ( rodata_section ) ;
	  else
	    insection ( data_section ) ;
	} 
	else if (dyn_init && isINITproc(id) && sysV_assembler) {
	  /* On solaris, this is easy.  Insert a call to the procedure
	     into the init segment */
	  insection (init_section);
	  fprintf(as_file,"\tcall %s\n",id);
	  outs("\tnop\n");
	  insection (text_section);
	}
	else{
	  insection ( text_section ) ;
	}
	outs ( "\t.global\t" ) ;
	outs ( id ) ;
	outnl () ;
      }
      if ( name ( stg ) != proc_tag && name(stg)!=general_proc_tag) {
	  /* evaluate all outer level constants */
	instore is ;
	long symdef = d->dec_u.dec_val.sym_number + 1 ;
#ifdef TDF_DIAG4
	struct dg_name_t *diag_props = d->dec_u.dec_val.dg_name ;
#else
	diag_global *diag_props = d->dec_u.dec_val.diag_info ;
#endif
	if ( isvar ( tg ) ) symdef = -symdef ;
	if ( diag_props ) {
		if (diag != DIAG_DWARF2) {
#ifdef STABS
			stab_global(diag_props, stg, id, extnamed);
#endif
		}
	}
	is = evaluated ( stg, symdef, 
		(!isvar (tg) || (d -> dec_u.dec_val.acc & f_constant)) ) ;
	if ( is.adval ) setvar ( tg ) ;
	if ( sysV_assembler ) {
	  outs ( "\t.type\t" ) ;
	  outs ( id ) ;
	  outs ( ",#object\n" ) ;
	  if ( !know_size ) {
	    outs ( "\t.size\t" ) ;
	    outs ( id ) ;
	    outs ( "," ) ;
	    outn ( shape_size ( sh ( stg ) ) / 8 ) ;
	    outnl () ;
	  }
	}
      }
    }
  }	

    /* translate procedures */
  for ( d = top_def ; d != NULL ; d = d->def_next ) {
    exp tg = d->dec_u.dec_val.dec_exp ;
    exp stg = son ( tg ) ;
    char *id = d->dec_u.dec_val.dec_id ;
    bool extnamed = ( bool ) d->dec_u.dec_val.extnamed ;

    if ( stg != NULL && shape_size (sh(stg)) == 0 && name(stg) == asm_tag) {
      if (props(stg) != 0)
	error(ERROR_INTERNAL, "~asm not in ~asm_sequence");
      check_asm_seq (son(stg), 1);
      insection ( text_section ) ;
      (void)code_here ( stg, tempregs, nowhere ) ;
      outnl ();
    }

    if ( no ( tg ) == 0 && !extnamed ) continue ;
    if ( stg != NULL ) {
      if ( name ( stg ) == proc_tag || name(stg)==general_proc_tag) {
	/* translate code for procedure */
	int proc_directive ;
	exp c = d->dec_u.dec_val.dec_exp ;
	prop p = procrecs [ no ( son ( c ) ) ].needsproc.prps ;
#ifdef TDF_DIAG4
	struct dg_name_t *diag_props = d->dec_u.dec_val.dg_name ;
#else
	diag_global *diag_props = d->dec_u.dec_val.diag_info ;
#endif
	insection ( text_section ) ;

	if(!sysV_assembler){
          if (gencompat) {
	    optim_level = (proc_may_have_callees(stg))?0:2;
          } else {
	    optim_level = (name(stg) == general_proc_tag)?0:2;
          }
	}
	
	/*
	  The .proc directive number tells Sun's assembler
	  optimiser which register are live at the end of the
	  procedure. We must get it right, or be conservative.
       */
	if ( p & longrealresult_bit ) {
	  proc_directive = 7 ;	    /* %f0 and %f1 */
	} 
	else if ( p & realresult_bit ) {
	  proc_directive = 6 ;	    /* %f0 */
	} 
	else if ( p & long_result_bit ) {
	  proc_directive = 0 ;	    /* compound */
	} 
	else if ( p & has_result_bit ) {
	  proc_directive = 4 ;	    /* %i0 or (leaf) %o0 */
	} 
	else {
	  proc_directive = 0 ;
	}
	outs ( "\t.proc\t" ) ;
	outn ( proc_directive ) ;
	outnl () ;
	if (do_comment) {
		if ( p & long_result_bit ) outs ( "!\tstruct result\n" ) ;
	}

	if ( diag != DIAG_NONE ) {
#if DWARF2
		if (diag == DIAG_DWARF2) {
			dw2_proc_start(stg, diag_props);
		}
#else
#ifdef STABS
		stab_proc(diag_props, stg, id, extnamed);
#endif
#endif
	}
	if ( optim_level >= 0 ) {
	  /* .optim must go after .proc */
	  if ( ( p & long_result_bit ) || ( p & dont_optimise ) ) {
	    outs ( "\t.optim\t\"-O0\"\n" ) ;
	  } 
	  else {
	    outs ( "\t.optim\t\"-O" ) ;
	    outn ( optim_level ) ;
	    outs ( "\"\n" ) ;
	  }
	}
	
	
	outs ( id ) ;
	outs ( ":\n" ) ;
	seed_label () ;		/* reset label sequence */
	settempregs ( stg ) ;	/* reset getreg sequence */
	/* generate code for this proc */

	proc_name = id;
	(void)code_here ( stg, tempregs, nowhere ) ;
	if ( sysV_assembler ) {
	  outs ( "\t.type\t" ) ;
	  outs ( id ) ;
	  outs ( ",#function\n" ) ;
	  outs ( "\t.size\t" ) ;
	  outs ( id ) ;
	  outs ( ",.-" ) ;
	  outs ( id ) ;
	  outnl () ;
	} 
	else {
		if (do_comment) {
		  outs ( "!\t.end\t" ) ;
		  outs ( id ) ;
		  outnl () ;
		}
	}
	if ( diag != DIAG_NONE ) {
#ifdef DWARF2
		if (diag == DIAG_DWARF2) {
			dw2_proc_end(stg);
		}
#else
#ifdef STABS
		stab_proc_end();
#endif
#endif
	}
      }
    } 
  }	


#ifdef DWARF2
  if (diag == DIAG_DWARF2) {
    dwarf2_postlude ();
  }
#endif
  return ;
}

