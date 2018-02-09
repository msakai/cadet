//
//  cegar.c
//  cadet
//
//  Created by Markus Rabe on 08.02.18.
//  Copyright © 2018 UC Berkeley. All rights reserved.
//

#include "domain.h"
#include "log.h"

#include <assert.h>


bool cegar_var_needs_to_be_set(Domain* d, unsigned var_id) {
    abortif(int_vector_get(d->is_used_in_lemma, var_id) == 0, "Variable not used in CEGAR lemma?");
    int satval = satsolver_deref(d->exists_solver, (int) var_id);
    abortif(satval == 0, "CEGAR lemma variable not set in SAT solver");
    
    Var* v = var_vector_get(d->qcnf->vars, var_id);
    vector* occs = satval > 0 ? &v->pos_occs : &v->neg_occs;
    int_vector* additional_assignments_var = int_vector_init();
    for (unsigned i = 0; i < vector_count(occs); i++) {
        Clause* c = vector_get(occs, i);
        if (! c->original || c->blocked) {
            continue;
        }
        bool c_satisfied_without = false;
        int can_be_satisfied_by_unset = 0;
        for (unsigned j = 0; j < c->size; j++) {
            int occ = c->occs[j];
            if (var_id == lit_to_var(occ)) {
                continue;
            }
            if (satsolver_deref(d->exists_solver, occ) == -1 || int_vector_get(d->is_used_in_lemma, lit_to_var(occ)) == 0) {
                continue;
            }
            
            if (satsolver_deref(d->exists_solver, occ) == 1 || int_vector_contains_sorted(d->additional_assignment, occ) || int_vector_contains_sorted(additional_assignments_var, occ)) {
                c_satisfied_without = true;
                break;
            } else {
                assert(satsolver_deref(d->exists_solver, occ) == 0);
                if (can_be_satisfied_by_unset == 0 && ! int_vector_contains_sorted(d->additional_assignment, -occ) && ! int_vector_contains_sorted(additional_assignments_var, - occ)) {
                    c_satisfied_without = true;
                    can_be_satisfied_by_unset = occ;
                }
            }
        }
        
        if (!c_satisfied_without) {
            int_vector_free(additional_assignments_var);
            return true;
        }
        if (can_be_satisfied_by_unset != 0) {
            d->cegar_stats.additional_assignments_num += 1;
            int_vector_add_sorted(additional_assignments_var, can_be_satisfied_by_unset);
        }
    }
    int_vector_add_all_sorted(d->additional_assignment, additional_assignments_var);
    if (int_vector_count(additional_assignments_var) > 0) {
        d->cegar_stats.successful_minimizations_by_additional_assignments += 1;
    }
    int_vector_free(additional_assignments_var);
    d->cegar_stats.successful_minimizations += 1;
    return false;
}

cadet_res domain_do_cegar_for_conflicting_assignment(C2* c2) {
    assert(domain_is_initialized(c2->skolem->domain));
    assert(c2->result == CADET_RESULT_UNKNOWN);
    assert(c2->state == C2_SKOLEM_CONFLICT);
    Domain* d = c2->skolem->domain;
    
    V3("Assuming: ");
    for (unsigned i = 0 ; i < int_vector_count(d->interface_vars); i++) {
        unsigned var_id = (unsigned) int_vector_get(d->interface_vars, i);
        int_vector_set(d->is_used_in_lemma, var_id, 1); // reset values
        
        int val = domain_get_cegar_val(c2->skolem, (int) var_id);
        satsolver_assume(d->exists_solver, val * (Lit) var_id);
        V3(" %d", val * (Lit) var_id);
    }
    V3("\n");
    
#ifdef DEBUG
    for (unsigned i = 0; i < var_vector_count(c2->qcnf->vars); i++) {
        Var* v = var_vector_get(c2->qcnf->vars, i);
        if (!v->original) {
            continue;
        }
        assert(int_vector_get(d->is_used_in_lemma, i) == 1);
    }
#endif
    
    if (satsolver_sat(d->exists_solver) == SATSOLVER_RESULT_SAT) {
        
        int_vector_reset(d->additional_assignment);
        int_vector* cube = int_vector_init();
        
        for (unsigned i = 0 ; i < int_vector_count(d->interface_vars); i++) {
            unsigned var_id = (unsigned) int_vector_get(d->interface_vars, i);
            int val = satsolver_deref(d->exists_solver, (int) var_id);
            
            if (cegar_var_needs_to_be_set(d, var_id)) {
                Lit lit = - val * (Lit) var_id;
                int_vector_add(cube, lit);
            } else {
                int_vector_set(d->is_used_in_lemma, var_id, 0);
            }
        }
        
        int_vector* existentials = NULL;
        if (c2->options->certify_SAT) {
            existentials = int_vector_init();
            for (unsigned var_id = 1; var_id < var_vector_count(c2->qcnf->vars); var_id++) {
                if (! skolem_is_deterministic(c2->skolem, var_id) || skolem_get_decision_lvl(c2->skolem, var_id) > 0) {
                    assert(qcnf_var_exists(c2->qcnf, var_id));
                    int val = satsolver_deref(d->exists_solver, (int) var_id);
                    if (val == 0 && int_vector_find_sorted(d->additional_assignment, - (int) var_id)) {
                        val = -1;
                    } else { // potentially (int) var_id is in additional_assignment
                        val = +1;  // default is +1
                    }
                    assert(val == -1 || val == +1);
                    int_vector_add(existentials, val * (int) var_id);
                }
            }
        }
        
        domain_completed_case(c2->skolem, cube, existentials, NULL);
        c2->skolem->domain->cegar_stats.recent_average_cube_size = (float) int_vector_count(cube) * (float) 0.1 + c2->skolem->domain->cegar_stats.recent_average_cube_size * (float) 0.9;
    } else {
        c2->state = C2_CEGAR_CONFLICT;
        c2->result = CADET_RESULT_UNSAT;
    }
    assert(c2->result != CADET_RESULT_SAT);
    return c2->result;
}

cadet_res domain_solve_2QBF_by_cegar(C2* c2, int rounds_num) {
    
    assert(domain_is_initialized(c2->skolem->domain));
    
    // solver loop
    while (c2->result == CADET_RESULT_UNKNOWN && rounds_num--) {
        if (satsolver_sat(c2->skolem->skolem) == SATSOLVER_RESULT_SAT) {
            domain_do_cegar_for_conflicting_assignment(c2);
        } else {
            c2->result = CADET_RESULT_SAT;
        }
    }
    return c2->result;
}

void do_cegar_if_effective(C2* c2) {
    assert(domain_is_initialized(c2->skolem->domain));
    unsigned i = 0;
    while (c2->result == CADET_RESULT_UNKNOWN &&
           c2->skolem->domain->cegar_stats.recent_average_cube_size < c2->skolem->domain->cegar_magic.cegar_effectiveness_threshold) {
        i++;
        domain_solve_2QBF_by_cegar(c2,1);
    }
    V1("Executed %u rounds of CEGAR.\n", i);
    if (c2->result == CADET_RESULT_UNKNOWN) {
        V1("CEGAR inconclusive, returning to normal mode.\n");
    } else {
        V1("CEGAR solved the problem: %d\n", c2->result);
    }
}

int domain_get_cegar_val(void* domain, Lit lit) {
    Skolem* s = (Skolem*) domain;
    int val = skolem_get_value_for_conflict_analysis(s, lit);
    if (val == 0) {
        val = 1;
    }
    assert(val == -1 || val == 1);
    return val;
}
