/* Helper class for handling a call with specific arguments.
   Copyright (C) 2020-2023 Free Software Foundation, Inc.
   Contributed by David Malcolm <dmalcolm@redhat.com>.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#define INCLUDE_MEMORY
#include "system.h"
#include "coretypes.h"
#include "tree.h"
#include "function.h"
#include "basic-block.h"
#include "gimple.h"
#include "diagnostic-core.h"
#include "analyzer/analyzer.h"
#include "analyzer/analyzer-logging.h"
#include "diagnostic.h"
#include "tree-diagnostic.h" /* for default_tree_printer.  */
#include "gimple-pretty-print.h"
#include "analyzer/region-model.h"
#include "analyzer/call-details.h"
#include "stringpool.h"
#include "attribs.h"

#if ENABLE_ANALYZER

namespace ana {

/* class call_details.  */

/* call_details's ctor.  */

call_details::call_details (const gcall *call, region_model *model,
			    region_model_context *ctxt)
: m_call (call), m_model (model), m_ctxt (ctxt),
  m_lhs_type (NULL_TREE), m_lhs_region (NULL)
{
  m_lhs_type = NULL_TREE;
  if (tree lhs = gimple_call_lhs (call))
    {
      m_lhs_region = model->get_lvalue (lhs, ctxt);
      m_lhs_type = TREE_TYPE (lhs);
    }
}

/* Get the manager from m_model.  */

region_model_manager *
call_details::get_manager () const
{
  return m_model->get_manager ();
}

/* Get any logger associated with this object.  */

logger *
call_details::get_logger () const
{
  if (m_ctxt)
    return m_ctxt->get_logger ();
  else
    return NULL;
}

/* Get any uncertainty_t associated with the region_model_context.  */

uncertainty_t *
call_details::get_uncertainty () const
{
  if (m_ctxt)
    return m_ctxt->get_uncertainty ();
  else
    return NULL;
}

/* If the callsite has a left-hand-side region, set it to RESULT
   and return true.
   Otherwise do nothing and return false.  */

bool
call_details::maybe_set_lhs (const svalue *result) const
{
  gcc_assert (result);
  if (m_lhs_region)
    {
      m_model->set_value (m_lhs_region, result, m_ctxt);
      return true;
    }
  else
    return false;
}

/* Return true if CD is known to be a call to a function with
   __attribute__((const)).  */

static bool
const_fn_p (const call_details &cd)
{
  tree fndecl = cd.get_fndecl_for_call ();
  if (!fndecl)
    return false;
  gcc_assert (DECL_P (fndecl));
  return TREE_READONLY (fndecl);
}

/* If this CD is known to be a call to a function with
   __attribute__((const)), attempt to get a const_fn_result_svalue
   based on the arguments, or return NULL otherwise.  */

static const svalue *
maybe_get_const_fn_result (const call_details &cd)
{
  if (!const_fn_p (cd))
    return NULL;

  unsigned num_args = cd.num_args ();
  if (num_args > const_fn_result_svalue::MAX_INPUTS)
    /* Too many arguments.  */
    return NULL;

  auto_vec<const svalue *> inputs (num_args);
  for (unsigned arg_idx = 0; arg_idx < num_args; arg_idx++)
    {
      const svalue *arg_sval = cd.get_arg_svalue (arg_idx);
      if (!arg_sval->can_have_associated_state_p ())
	return NULL;
      inputs.quick_push (arg_sval);
    }

  region_model_manager *mgr = cd.get_manager ();
  const svalue *sval
    = mgr->get_or_create_const_fn_result_svalue (cd.get_lhs_type (),
						 cd.get_fndecl_for_call (),
						 inputs);
  return sval;
}

/* Look for attribute "alloc_size" on the called function and, if found,
   return a symbolic value of type size_type_node for the allocation size
   based on the call's parameters.
   Otherwise, return null.  */

static const svalue *
get_result_size_in_bytes (const call_details &cd)
{
  const tree attr = cd.lookup_function_attribute ("alloc_size");
  if (!attr)
    return nullptr;

  const tree atval_1 = TREE_VALUE (attr);
  if (!atval_1)
    return nullptr;

  unsigned argidx1 = TREE_INT_CST_LOW (TREE_VALUE (atval_1)) - 1;
  if (cd.num_args () <= argidx1)
    return nullptr;

  const svalue *sval_arg1 = cd.get_arg_svalue (argidx1);

  if (const tree atval_2 = TREE_CHAIN (atval_1))
    {
      /* Two arguments.  */
      unsigned argidx2 = TREE_INT_CST_LOW (TREE_VALUE (atval_2)) - 1;
      if (cd.num_args () <= argidx2)
	return nullptr;
      const svalue *sval_arg2 = cd.get_arg_svalue (argidx2);
      /* TODO: ideally we shouldn't need this cast here;
	 see PR analyzer/110902.  */
      return cd.get_manager ()->get_or_create_cast
	(size_type_node,
	 cd.get_manager ()->get_or_create_binop (size_type_node,
						 MULT_EXPR,
						 sval_arg1, sval_arg2));
    }
  else
    /* Single argument.  */
    return cd.get_manager ()->get_or_create_cast (size_type_node, sval_arg1);
}

/* If this call has an LHS, assign a value to it based on attributes
   of the function:
   - if __attribute__((const)), use a const_fn_result_svalue,
   - if __attribute__((malloc)), use a heap-allocated region with
   unknown content
   - otherwise, use a conjured_svalue.

   If __attribute__((alloc_size), set the dynamic extents on the region
   pointed to.  */

void
call_details::set_any_lhs_with_defaults () const
{
  if (!m_lhs_region)
    return;

  const svalue *sval = maybe_get_const_fn_result (*this);
  if (!sval)
    {
      region_model_manager *mgr = get_manager ();
      if (lookup_function_attribute ("malloc"))
	{
	  const region *new_reg
	    = m_model->get_or_create_region_for_heap_alloc (NULL, m_ctxt);
	  m_model->mark_region_as_unknown (new_reg, NULL);
	  sval = mgr->get_ptr_svalue (get_lhs_type (), new_reg);
	}
      else
	/* For the common case of functions without __attribute__((const)),
	   use a conjured value, and purge any prior state involving that
	   value (in case this is in a loop).  */
	sval = get_or_create_conjured_svalue (m_lhs_region);
      if (const svalue *size_in_bytes = get_result_size_in_bytes (*this))
	{
	  const region *reg
	    = m_model->deref_rvalue (sval, NULL_TREE, m_ctxt, false);
	  m_model->set_dynamic_extents (reg, size_in_bytes, m_ctxt);
	}
    }
  maybe_set_lhs (sval);
}

/* Return the number of arguments used by the call statement.  */

unsigned
call_details::num_args () const
{
  return gimple_call_num_args (m_call);
}

/* Return true if argument IDX is a size_t (or compatible with it).  */

bool
call_details::arg_is_size_p (unsigned idx) const
{
  return types_compatible_p (get_arg_type (idx), size_type_node);
}

/* Get the location of the call statement.  */

location_t
call_details::get_location () const
{
  return m_call->location;
}

/* Get argument IDX at the callsite as a tree.  */

tree
call_details::get_arg_tree (unsigned idx) const
{
  return gimple_call_arg (m_call, idx);
}

/* Get the type of argument IDX.  */

tree
call_details::get_arg_type (unsigned idx) const
{
  return TREE_TYPE (gimple_call_arg (m_call, idx));
}

/* Get argument IDX at the callsite as an svalue.  */

const svalue *
call_details::get_arg_svalue (unsigned idx) const
{
  tree arg = get_arg_tree (idx);
  return m_model->get_rvalue (arg, m_ctxt);
}

/* Attempt to get the string literal for argument IDX, or return NULL
   otherwise.
   For use when implementing "__analyzer_*" functions that take
   string literals.  */

const char *
call_details::get_arg_string_literal (unsigned idx) const
{
  const svalue *str_arg = get_arg_svalue (idx);
  if (const region *pointee = str_arg->maybe_get_region ())
    if (const string_region *string_reg = pointee->dyn_cast_string_region ())
      {
	tree string_cst = string_reg->get_string_cst ();
	return TREE_STRING_POINTER (string_cst);
      }
  return NULL;
}

/* Attempt to get the fndecl used at this call, if known, or NULL_TREE
   otherwise.  */

tree
call_details::get_fndecl_for_call () const
{
  return m_model->get_fndecl_for_call (m_call, m_ctxt);
}

/* Dump a multiline representation of this call to PP.  */

void
call_details::dump_to_pp (pretty_printer *pp, bool simple) const
{
  pp_string (pp, "gcall: ");
  pp_gimple_stmt_1 (pp, m_call, 0 /* spc */, TDF_NONE /* flags */);
  pp_newline (pp);
  pp_string (pp, "return region: ");
  if (m_lhs_region)
    m_lhs_region->dump_to_pp (pp, simple);
  else
    pp_string (pp, "NULL");
  pp_newline (pp);
  for (unsigned i = 0; i < gimple_call_num_args (m_call); i++)
    {
      const svalue *arg_sval = get_arg_svalue (i);
      pp_printf (pp, "arg %i: ", i);
      arg_sval->dump_to_pp (pp, simple);
      pp_newline (pp);
    }
}

/* Dump a multiline representation of this call to stderr.  */

DEBUG_FUNCTION void
call_details::dump (bool simple) const
{
  pretty_printer pp;
  pp_format_decoder (&pp) = default_tree_printer;
  pp_show_color (&pp) = pp_show_color (global_dc->printer);
  pp.buffer->stream = stderr;
  dump_to_pp (&pp, simple);
  pp_flush (&pp);
}

/* Get a conjured_svalue for this call for REG,
   and purge any state already relating to that conjured_svalue.  */

const svalue *
call_details::get_or_create_conjured_svalue (const region *reg) const
{
  region_model_manager *mgr = m_model->get_manager ();
  return mgr->get_or_create_conjured_svalue (reg->get_type (), m_call, reg,
					     conjured_purge (m_model, m_ctxt));
}

/* Look for a function attribute with name ATTR_NAME on the called
   function (or on its type).
   Return the attribute if one is found, otherwise return NULL_TREE.  */

tree
call_details::lookup_function_attribute (const char *attr_name) const
{
  tree allocfntype;
  if (tree fndecl = get_fndecl_for_call ())
    allocfntype = TREE_TYPE (fndecl);
  else
    allocfntype = gimple_call_fntype (m_call);

  if (!allocfntype)
    return NULL_TREE;

  return lookup_attribute (attr_name, TYPE_ATTRIBUTES (allocfntype));
}

} // namespace ana

#endif /* #if ENABLE_ANALYZER */
