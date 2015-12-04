/*
 * Copyright 2011-2015 by Emese Revfy <re.emese@gmail.com>
 * Licensed under the GPL v2, or (at your option) v3
 *
 * Homepage:
 * https://github.com/ephox-gcc-plugins/size_overflow
 *
 * Documentation:
 * http://forums.grsecurity.net/viewtopic.php?f=7&t=3043
 *
 * This plugin recomputes expressions of function arguments marked by a size_overflow attribute
 * with double integer precision (DImode/TImode for 32/64 bit integer types).
 * The recomputed argument is checked against TYPE_MAX and an event is logged on overflow and the triggering process is killed.
 *
 * Usage:
 * $ make
 * $ make run
 */

#include "size_overflow.h"
struct interesting_stmts;
typedef struct interesting_stmts * interesting_stmts_t;

struct interesting_stmts {
	struct interesting_stmts *next;
	next_interesting_function_t next_node;
	gimple first_stmt;
	tree orig_node;
	unsigned int num;
};

static tree cast_to_orig_type(struct visited *visited, gimple stmt, const_tree orig_node, tree new_node)
{
	const_gimple assign;
	tree orig_type = TREE_TYPE(orig_node);
	gimple_stmt_iterator gsi = gsi_for_stmt(stmt);

	assign = build_cast_stmt(visited, orig_type, new_node, CREATE_NEW_VAR, &gsi, BEFORE_STMT, false);
	return get_lhs(assign);
}

static void change_size_overflow_asm_input(gasm *stmt, tree new_input)
{
	tree list;

	gcc_assert(is_size_overflow_insert_check_asm(stmt));

	list = build_tree_list(NULL_TREE, build_string(3, "rm"));
	list = chainon(NULL_TREE, build_tree_list(list, new_input));
	gimple_asm_set_input_op(stmt, 0, list);
}

static void change_field_write_rhs(gassign *assign, const_tree orig_rhs, tree new_rhs)
{
	const_tree rhs1, rhs2, rhs3 = NULL_TREE;

	rhs1 = gimple_assign_rhs1(assign);
	if (rhs1 == orig_rhs) {
		gimple_assign_set_rhs1(assign, new_rhs);
		return;
	}

	rhs2 = gimple_assign_rhs2(assign);
	if (rhs2 == orig_rhs) {
		gimple_assign_set_rhs2(assign, new_rhs);
		return;
	}

#if BUILDING_GCC_VERSION >= 4006
	rhs3 = gimple_assign_rhs3(assign);
	if (rhs3 == orig_rhs) {
		gimple_assign_set_rhs3(assign, new_rhs);
		return;
	}
#endif

	debug_gimple_stmt(assign);
	fprintf(stderr, "orig_rhs:\n");
	debug_tree((tree)orig_rhs);
	fprintf(stderr, "rhs1:\n");
	debug_tree((tree)rhs1);
	fprintf(stderr, "rhs2:\n");
	debug_tree((tree)rhs2);
	fprintf(stderr, "rhs3:\n");
	debug_tree((tree)rhs3);
	gcc_unreachable();
}

static void change_orig_node(struct visited *visited, gimple stmt, const_tree orig_node, tree new_node, unsigned int num)
{
	tree cast_lhs = cast_to_orig_type(visited, stmt, orig_node, new_node);

	switch (gimple_code(stmt)) {
	case GIMPLE_RETURN:
		gimple_return_set_retval(as_a_greturn(stmt), cast_lhs);
		break;
	case GIMPLE_CALL:
		gimple_call_set_arg(as_a_gcall(stmt), num - 1, cast_lhs);
		break;
	case GIMPLE_ASM:
		change_size_overflow_asm_input(as_a_gasm(stmt), cast_lhs);
		break;
	case GIMPLE_ASSIGN:
		change_field_write_rhs(as_a_gassign(stmt), orig_node, cast_lhs);
		break;
	default:
		debug_gimple_stmt(stmt);
		gcc_unreachable();
	}

	update_stmt(stmt);
}

// e.g., 3.8.2, 64, arch/x86/ia32/ia32_signal.c copy_siginfo_from_user32(): compat_ptr() u32 max
static bool skip_asm_cast(const_tree arg)
{
	gimple def_stmt = get_def_stmt(arg);

	if (!def_stmt || !gimple_assign_cast_p(def_stmt))
		return false;

	def_stmt = get_def_stmt(gimple_assign_rhs1(def_stmt));
	if (is_size_overflow_asm(def_stmt))
		return false;
	return def_stmt && gimple_code(def_stmt) == GIMPLE_ASM;
}

static interesting_stmts_t create_interesting_stmts(interesting_stmts_t head, next_interesting_function_t next_node, tree orig_node, gimple first_stmt, unsigned int num)
{
	interesting_stmts_t new_node;

	new_node = (interesting_stmts_t )xmalloc(sizeof(*new_node));
	new_node->first_stmt = first_stmt;
	new_node->num = num;
	new_node->orig_node = orig_node;
	new_node->next = head;
	new_node->next_node = next_node;
	return new_node;
}

static void free_interesting_stmts(interesting_stmts_t head)
{
	while (head) {
		interesting_stmts_t cur = head->next;
		free(head);
		head = cur;
	}
}

/* This function calls the main recursion function (expand) that duplicates the stmts. Before that it checks the intentional_overflow attribute,
 * it decides whether the duplication is necessary or not. After expand() it changes the orig node to the duplicated node
 * in the original stmt (first stmt) and it inserts the overflow check for the arg of the callee or for the return value.
 */
static interesting_stmts_t search_interesting_stmt(interesting_stmts_t head, next_interesting_function_t next_node, gimple first_stmt, tree orig_node, unsigned int num)
{
	enum tree_code orig_code;

	gcc_assert(orig_node != NULL_TREE);

	if (is_gimple_constant(orig_node))
		return head;

	orig_code = TREE_CODE(orig_node);
	gcc_assert(orig_code != FIELD_DECL && orig_code != FUNCTION_DECL);

	if (skip_types(orig_node))
		return head;

	// find a defining marked caller argument or struct field for arg
	if (check_intentional_size_overflow_asm_and_attribute(orig_node) != MARK_NO)
		return head;

	if (SSA_NAME_IS_DEFAULT_DEF(orig_node))
		return head;

	if (skip_asm_cast(orig_node))
		return head;

	return create_interesting_stmts(head, next_node, orig_node, first_stmt, num);
}

static void handle_interesting_stmt(struct visited *visited, interesting_stmts_t head)
{
	interesting_stmts_t cur;

	for (cur = head; cur; cur = cur->next) {
		tree new_node;

		new_node = expand(visited, cur->next_node, cur->orig_node);
		if (new_node == NULL_TREE)
			continue;

		change_orig_node(visited, cur->first_stmt, cur->orig_node, new_node, cur->num);
		check_size_overflow(cur->next_node, cur->first_stmt, TREE_TYPE(new_node), new_node, cur->orig_node, BEFORE_STMT);
	}
}

static next_interesting_function_t get_interesting_function_next_node(tree decl, unsigned int num)
{
	next_interesting_function_t next_node;
	const struct size_overflow_hash *so_hash, *disable_so_hash;
	struct fn_raw_data raw_data;

	raw_data.decl = decl;
	raw_data.decl_str = DECL_NAME_POINTER(decl);
	raw_data.num = num;
	raw_data.marked = YES_SO_MARK;

	next_node = get_global_next_interesting_function_entry_with_hash(&raw_data);
	if (next_node && next_node->marked == ERROR_CODE_SO_MARK)
		return NULL;
	if (next_node && next_node->marked != NO_SO_MARK)
		return next_node;

	disable_so_hash = get_size_overflow_hash_entry_tree(raw_data.decl, raw_data.num, ONLY_DISABLE_SO);
	if (disable_so_hash != NULL)
		return NULL;

	so_hash = get_size_overflow_hash_entry_tree(raw_data.decl, raw_data.num, ONLY_SO);
	if (so_hash)
		return get_and_create_next_node_from_global_next_nodes(&raw_data, NULL);
	return NULL;
}

tree handle_fnptr_assign(const_gimple stmt)
{
	tree field, rhs, op0;
	const_tree op0_type;
	enum tree_code rhs_code;

	// TODO skip binary assignments for now (fs/sync.c _591 = __bpf_call_base + _590;)
	if (gimple_num_ops(stmt) != 2)
		return NULL_TREE;

	gcc_assert(gimple_num_ops(stmt) == 2);
	// TODO skip asm_stmt for now
	if (gimple_code(stmt) == GIMPLE_ASM)
		return NULL_TREE;
	rhs = gimple_assign_rhs1(stmt);
	if (is_gimple_constant(rhs))
		return NULL_TREE;

	rhs_code = TREE_CODE(rhs);
	if (rhs_code == VAR_DECL)
		return rhs;

	switch (rhs_code) {
	case ADDR_EXPR:
		op0 = TREE_OPERAND(rhs, 0);
		gcc_assert(TREE_CODE(op0) == FUNCTION_DECL);
		return op0;
	case COMPONENT_REF:
		break;
	// TODO skip array_ref for now
	case ARRAY_REF:
		return NULL_TREE;
	// TODO skip ssa_name because it can lead to parm_decl
	case SSA_NAME:
		return NULL_TREE;
	// TODO skip mem_ref and indirect_ref for now
#if BUILDING_GCC_VERSION >= 4006
	case MEM_REF:
#endif
	case INDIRECT_REF:
		return NULL_TREE;
	default:
		debug_tree(rhs);
		debug_gimple_stmt((gimple)stmt);
		gcc_unreachable();
	}

	op0 = TREE_OPERAND(rhs, 0);
	switch (TREE_CODE(op0)) {
	// TODO skip array_ref and parm_decl for now
	case ARRAY_REF:
	case PARM_DECL:
		return NULL_TREE;
	case COMPONENT_REF:
#if BUILDING_GCC_VERSION >= 4006
	case MEM_REF:
#endif
	case INDIRECT_REF:
	case VAR_DECL:
		break;
	default:
		debug_tree(op0);
		gcc_unreachable();
	}

	op0_type = TREE_TYPE(op0);
	// TODO skip unions for now
	if (TREE_CODE(op0_type) == UNION_TYPE)
		return NULL_TREE;
	gcc_assert(TREE_CODE(op0_type) == RECORD_TYPE);

	field = TREE_OPERAND(rhs, 1);
	gcc_assert(TREE_CODE(field) == FIELD_DECL);
	return field;
}

static tree get_fn_or_fnptr_decl(const gcall *call_stmt)
{
	const_tree fnptr;
	const_gimple def_stmt;
	tree decl = gimple_call_fndecl(call_stmt);

	if (decl != NULL_TREE)
		return decl;

	fnptr = gimple_call_fn(call_stmt);
	// !!! assertot kell irni 0-ra, mert csak az lehet ott
	if (is_gimple_constant(fnptr))
		return NULL_TREE;
	def_stmt = get_fnptr_def_stmt(fnptr);
	return handle_fnptr_assign(def_stmt);
}

// Start stmt duplication on marked function parameters
static interesting_stmts_t search_interesting_calls(interesting_stmts_t head, gcall *call_stmt)
{
	tree decl;
	unsigned int i, len;

	len = gimple_call_num_args(call_stmt);
	if (len == 0)
		return head;

	decl = get_fn_or_fnptr_decl(call_stmt);
	if (decl == NULL_TREE)
		return head;

	for (i = 0; i < len; i++) {
		tree arg;
		next_interesting_function_t next_node;

		arg = gimple_call_arg(call_stmt, i);
		if (is_gimple_constant(arg))
			continue;
		if (skip_types(arg))
			continue;
		next_node = get_interesting_function_next_node(decl, i + 1);
		if (next_node)
			head = search_interesting_stmt(head, next_node, call_stmt, arg, i + 1);
	}

	return head;
}

// Find assignements to structure fields and vardecls
static interesting_stmts_t search_interesting_structs_vardecls(interesting_stmts_t head, gassign *assign)
{
	enum intentional_mark mark;
	next_interesting_function_t next_node;
	tree rhs1, rhs2, lhs, decl;
#if BUILDING_GCC_VERSION >= 4006
	tree rhs3;
#endif

	lhs = gimple_assign_lhs(assign);

	if (VAR_P(lhs))
		decl = lhs;
	else
		decl = get_ref_field(lhs);
	if (decl == NULL_TREE)
		return head;
	if (DECL_NAME(decl) == NULL_TREE)
		return head;

	next_node = get_interesting_function_next_node(decl, 0);
	if (!next_node)
		return head;

	mark = get_intentional_attr_type(decl);
	if (mark != MARK_NO)
		return head;

	if (is_intentional_truncation(assign))
		return head;

	rhs1 = gimple_assign_rhs1(assign);
	head = search_interesting_stmt(head, next_node, assign, rhs1, 0);

	rhs2 = gimple_assign_rhs2(assign);
	if (rhs2)
		head = search_interesting_stmt(head, next_node, assign, rhs2, 0);

#if BUILDING_GCC_VERSION >= 4006
	rhs3 = gimple_assign_rhs3(assign);
	if (rhs3)
		head = search_interesting_stmt(head, next_node, assign, rhs3, 0);
#endif
	return head;
}

static next_interesting_function_t create_so_asm_next_interesting_function_node(const gasm *stmt)
{
	next_interesting_function_t next_node;
	struct fn_raw_data raw_data;

	raw_data.decl = NULL_TREE;
	raw_data.decl_str = gimple_asm_string(stmt);
	raw_data.context = "attr";
	raw_data.hash = 0;
	raw_data.num = 0;
	raw_data.marked = ASM_STMT_SO_MARK;

	next_node = get_global_next_interesting_function_entry(&raw_data);
	if (next_node)
		return next_node;
	next_node = create_new_next_interesting_entry(&raw_data, NULL);
	gcc_assert(next_node);

	add_to_global_next_interesting_function(next_node);
	return next_node;
}

// Collect interesting stmts for duplication
static void search_interesting_stmts(struct visited *visited)
{
	basic_block bb;
	next_interesting_function_t next_node_ret;
	interesting_stmts_t head = NULL;

	next_node_ret = get_interesting_function_next_node(current_function_decl, 0);

	FOR_ALL_BB_FN(bb, cfun) {
		gimple_stmt_iterator gsi;

		for (gsi = gsi_start_bb(bb); !gsi_end_p(gsi); gsi_next(&gsi)) {
			tree first_node;
			gimple stmt = gsi_stmt(gsi);

			switch (gimple_code(stmt)) {
			case GIMPLE_ASM: {
				next_interesting_function_t next_node;
				const gasm *asm_stmt = as_a_gasm(stmt);

				if (!is_size_overflow_insert_check_asm(asm_stmt))
					continue;
				next_node = create_so_asm_next_interesting_function_node(asm_stmt);
				first_node = get_size_overflow_asm_input(asm_stmt);
				head = search_interesting_stmt(head, next_node, stmt, first_node, 0);
				break;
			}
			case GIMPLE_RETURN:
				if (!next_node_ret || next_node_ret->marked == ASM_STMT_SO_MARK)
					continue;
				first_node = gimple_return_retval(as_a_greturn(stmt));
				if (first_node == NULL_TREE)
					break;
				head = search_interesting_stmt(head, next_node_ret, stmt, first_node, 0);
				break;
			case GIMPLE_CALL:
				head = search_interesting_calls(head, as_a_gcall(stmt));
				break;
			case GIMPLE_ASSIGN:
				/* !!! TODO LTO modeban nincs duplikalas a globalis valtozora, mert a tree mergek
				 * utan mar nem lehet megkulonboztetni attol a globalis valtozotol, aminek a scopeja csak a file
				 * igy a context nem vardecl lesz, hanem vardecl_filenev. De execute-ban kiirja, ha hianyzik a hash tablabol
				 * IPA-ban van duplikalas.
				 */
				head = search_interesting_structs_vardecls(head, as_a_gassign(stmt));
				break;
			default:
				break;
			}
		}
	}

	handle_interesting_stmt(visited, head);
	free_interesting_stmts(head);
}

static struct visited *create_visited(void)
{
	struct visited *new_node;

	new_node = (struct visited *)xmalloc(sizeof(*new_node));
	new_node->stmts = pointer_set_create();
	new_node->my_stmts = pointer_set_create();
	new_node->skip_expr_casts = pointer_set_create();
	new_node->no_cast_check = pointer_set_create();
	return new_node;
}

static void free_visited(struct visited *visited)
{
	pointer_set_destroy(visited->stmts);
	pointer_set_destroy(visited->my_stmts);
	pointer_set_destroy(visited->skip_expr_casts);
	pointer_set_destroy(visited->no_cast_check);

	free(visited);
}

// Remove the size_overflow asm stmt and create an assignment from the input and output of the asm
static void replace_size_overflow_asm_with_assign(gasm *asm_stmt, tree lhs, tree rhs)
{
	gassign *assign;
	gimple_stmt_iterator gsi;

	// already removed
	if (gimple_bb(asm_stmt) == NULL)
		return;
	gsi = gsi_for_stmt(asm_stmt);

	assign = gimple_build_assign(lhs, rhs);
	gsi_insert_before(&gsi, assign, GSI_SAME_STMT);
	SSA_NAME_DEF_STMT(lhs) = assign;

	gsi_remove(&gsi, true);
}

// Replace our asm stmts with assignments (they are no longer needed and may interfere with later optimizations)
static void remove_size_overflow_asm(gimple stmt)
{
	gimple_stmt_iterator gsi;
	tree input, output;

	if (!is_size_overflow_asm(stmt))
		return;

	if (gimple_asm_noutputs(as_a_gasm(stmt)) == 0) {
		gsi = gsi_for_stmt(stmt);

		ipa_remove_stmt_references(cgraph_get_node(current_function_decl), stmt);
		gsi_remove(&gsi, true);
		return;
	}

	input = gimple_asm_input_op(as_a_gasm(stmt), 0);
	output = gimple_asm_output_op(as_a_gasm(stmt), 0);
	replace_size_overflow_asm_with_assign(as_a_gasm(stmt), TREE_VALUE(output), TREE_VALUE(input));
}

static void remove_all_size_overflow_asm(void)
{
	basic_block bb;

	FOR_ALL_BB_FN(bb, cfun) {
		gimple_stmt_iterator si;

		for (si = gsi_start_bb(bb); !gsi_end_p(si); gsi_next(&si))
			remove_size_overflow_asm(gsi_stmt(si));
	}
}

unsigned int size_overflow_transform(struct cgraph_node *node __unused)
{
	struct visited *visited;

#if BUILDING_GCC_VERSION >= 4008
	if (dump_file) {
		fprintf(dump_file, "BEFORE TRANSFORM -------------------------\n");
		size_overflow_dump_function(dump_file, node);
	}
#endif
	visited = create_visited();
	set_dominance_info();

	search_interesting_stmts(visited);

	remove_all_size_overflow_asm();

	unset_dominance_info();
	free_visited(visited);

#if BUILDING_GCC_VERSION >= 4008
	if (dump_file) {
		fprintf(dump_file, "AFTER TRANSFORM -------------------------\n");
		size_overflow_dump_function(dump_file, node);
	}
#endif
	return TODO_dump_func | TODO_verify_stmts | TODO_remove_unused_locals | TODO_update_ssa_no_phi | TODO_ggc_collect | TODO_verify_flow;
}
