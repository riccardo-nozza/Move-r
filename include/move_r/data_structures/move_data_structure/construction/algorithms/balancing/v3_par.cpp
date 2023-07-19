template <typename uint_t>
inline move_data_structure_phi<uint_t>::construction::tout_node_t_v2v3v4* move_data_structure_phi<uint_t>::construction::balance_upto_v3_par(
    lin_node_t_v2v3v4* ln_IpA,
    tout_node_t_v2v3v4* tn_J,
    tout_node_t_v2v3v4* tn_J_,
    uint_t q_u,
    uint_t p_cur,
    uint_t *i_
) {
    // Index in [0..p-1] of the current thread.
    uint16_t i_p = omp_get_thread_num();

    uint_t p_j = tn_J->v.v.first;
    uint_t q_j = tn_J->v.v.second;
    uint_t d_j = tn_J_->v.v.second - q_j;

    // d = p_{i+2a} - q_j is the maximum integer, so that [q_j, q_j + d) has a incoming edges in the permutation graph.
    uint_t d = ln_IpA->v.first - q_j;

    // Create the pair (p_j + d, q_j + d), which creates two new input intervals [p_j, p_j + d) and [p_j + d, p_j + d_j).
    tout_node_t_v2v3v4 *tn_NEW = new_nodes_2v3v4[i_p].emplace_back(tout_node_t_v2v3v4(lin_node_t_v2v3v4(pair_t{p_j + d, q_j + d})));
    T_out_v2v3v4[i_p].insert_node_in(tn_NEW,tn_J);

    if (!(s[i_p] <= p_j + d && p_j + d < s[i_p+1])) {
        // If the new pair must be inserted in L_in_v2v3v4[i_p_] of another thread i_p_ != i_p, find i_p_ with a binary search.
        uint16_t l = 0;
        uint16_t r = p-1;
        uint16_t m;
        while (l != r) {
            m = l+(r-l)/2+1;
            if (p_j + d >= s[m]) {
                l = m;
            } else {
                r = m-1;
            }
        }
        uint16_t i_p_ = l;

        Q_v3[i_p_][i_p].emplace(q_node_t_v34{&tn_NEW->v,&tn_J->v});
    } else {
        // Else insert it in L_in_v2v3v4[i_p].
        L_in_v2v3v4[i_p].insert_after_node(&tn_NEW->v,&tn_J->v);

        if (p_j + d < q_u) {
            if (p_j + d < q_j || q_j + d_j <= p_j + d) {
                tout_node_t_v2v3v4 *tn_Y = T_out_v2v3v4[i_p].maximum_leq(lin_node_t_v2v3v4(pair_t{0,p_j + d}));
                uint_t q_y = tn_Y->v.v.second;

                // find the output interval starting after [q_y, q_y + d_y)
                tout_node_t_v2v3v4 *tn_Y_nxt = tn_Y->nxt();

                // find the first input interval [p_z, p_z + d_z), that is connected to [q_y, q_y + d_y) in the permutation graph
                lin_node_t_v2v3v4 *ln_Z = &tn_NEW->v;
                uint_t i__ = 1;
                while (ln_Z->pr != NULL && ln_Z->pr->v.first >= q_y) {
                    ln_Z = ln_Z->pr;
                    i__++;
                }
                ln_Z = &tn_NEW->v;
                
                lin_node_t_v2v3v4 *ln_ZpA = is_a_heavy_v2v3v4(&ln_Z,&i__,tn_Y,tn_Y_nxt);
                if (ln_ZpA != NULL) {
                    balance_upto_v3_par(ln_ZpA,tn_Y,tn_Y_nxt,q_u,p_cur,i_);
                }
            }
        } else if (p_j + d < p_cur) {
            (*i_)++;
        }
    }
    return tn_NEW;
}

/**
 * @brief balances the disjoint interval sequence in L_in_v2v3v4[0..p-1] and T_out_v2v3v4[0..p-1] in parallel
 */
template <typename uint_t>
void move_data_structure_phi<uint_t>::construction::balance_v3_par() {
    if (log) log_message("balancing (phase 1)");
    
    Q_v3.resize(p,std::vector<std::queue<q_node_t_v34>>(p));
    Q_v3_.resize(p,std::vector<std::queue<q_node_t_v34>>(p));

    uint8_t not_done = 0;

    #pragma omp parallel num_threads(p)
    {
        // Index in [0..p-1] of the current thread.
        uint16_t i_p = omp_get_thread_num();

        // points to to the pair (p_i,q_i).
        lin_node_t_v2v3v4 *ln_I = L_in_v2v3v4[i_p].head();
        // points to the pair (p_j,q_j).
        typename tout_t_v2v3v4::avl_it it_outp_cur = T_out_v2v3v4[i_p].iterator();
        // points to the pair (p_{j'},q_{j'}), where q_j + d_j = q_{j'}.
        typename tout_t_v2v3v4::avl_it it_outp_nxt = T_out_v2v3v4[i_p].iterator(T_out_v2v3v4[i_p].second_smallest());

        // temporary variables
        lin_node_t_v2v3v4 *ln_IpA;
        uint_t i_ = 1;

        // At the start of each iteration, [p_i, p_i + d_i) is the first input interval connected to [q_j, q_j + d_j) in the permutation graph
        bool stop = false;
        while (!stop) {
            ln_IpA = is_a_heavy_v2v3v4(&ln_I,&i_,it_outp_cur.current(),it_outp_nxt.current());

            // If [q_j, q_j + d_j) is a-heavy, balance it and all output intervals starting before it, that might get a-heavy in the process.
            if (ln_IpA != NULL) {
                it_outp_cur.set(balance_upto_v3_par(ln_IpA,it_outp_cur.current(),it_outp_nxt.current(),it_outp_cur.current()->v.v.second,ln_I->v.first,&i_));
                continue;
            }

            // Find the next output interval with an incoming edge in the permutation graph and the first input interval connected to it.
            do {
                if (!it_outp_nxt.has_next()) {stop = true; break;}
                it_outp_cur.set(it_outp_nxt.current());
                it_outp_nxt.next();
                while (ln_I->v.first < it_outp_cur.current()->v.v.second) {
                    if (ln_I->sc == NULL) {stop = true; break;}
                    ln_I = ln_I->sc;
                }
            } while (!stop && ln_I->v.first >= it_outp_nxt.current()->v.v.second);
            i_ = 1;
        }
    }

    if (log) {
        if (mf != NULL) *mf << " time_balance_phase_1=" << time_diff_ns(time);
        time = log_runtime(time);
        log_message("balancing (phase 2)");
    }

    #pragma omp parallel num_threads(p)
    {
        // Index in [0..p-1] of the current thread.
        uint16_t i_p = omp_get_thread_num();

        // temporary variables
        lin_node_t_v2v3v4 *ln_I,*ln_Im1,*ln_Z,*ln_ZpA;
        tout_node_t_v2v3v4 *tn_Y,*tn_Y_nxt;
        uint_t i_ = 1;
        uint_t q_y;

        while (true) {
            #pragma omp barrier

            #pragma omp single
            {
                std::swap(Q_v3,Q_v3_);
                not_done = 0;
            }

            #pragma omp barrier

            for (uint16_t i=0; i<p; i++) {
                if (!Q_v3_[i_p][i].empty()) {
                    not_done = 1;
                    break;
                }
            }

            #pragma omp barrier

            if (not_done == 0) {
                break;
            }

            for (uint16_t i=0; i<p; i++) {
                while (!Q_v3_[i_p][i].empty()) {
                    ln_I = Q_v3_[i_p][i].front().first;
                    ln_Im1 = Q_v3_[i_p][i].front().second;
                    Q_v3_[i_p][i].pop();

                    L_in_v2v3v4[i_p].insert_after_node(ln_I,ln_Im1);

                    // check if an output interval could have become a-heavy by inserting the new pair
                    tn_Y = T_out_v2v3v4[i_p].maximum_leq(lin_node_t_v2v3v4(pair_t{0,ln_I->v.first}));
                    q_y = tn_Y->v.v.second;

                    // find the output interval starting after [q_y, q_y + d_y)
                    tn_Y_nxt = tn_Y->nxt();

                    // find the first input interval [p_z, p_z + d_z), that is connected to [q_y, q_y + d_y) in the permutation graph
                    ln_Z = ln_I;
                    i_ = 1;
                    while (ln_Z->pr != NULL && ln_Z->pr->v.first >= q_y) {
                        ln_Z = ln_Z->pr;
                        i_++;
                    }
                    ln_Z = ln_I;

                    ln_ZpA = is_a_heavy_v2v3v4(&ln_Z,&i_,tn_Y,tn_Y_nxt);
                    if (ln_ZpA != NULL) {
                        balance_upto_v3_par(ln_ZpA,tn_Y,tn_Y_nxt,s[i_p+1],s[i_p+1],&i_);
                    }
                }
            }
        }
    }
    
    Q_v3.clear();
    Q_v3.shrink_to_fit();

    Q_v3_.clear();
    Q_v3_.shrink_to_fit();

    if (log) {
        if (mf != NULL) *mf << " time_balance_phase_2=" << time_diff_ns(time);
        time = log_runtime(time);
    }
}