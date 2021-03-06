/*
 *   Copyright (C) 2015-2016 Mateusz Łącki and Michał Startek.
 *
 *   This file is part of IsoSpec.
 *
 *   IsoSpec is free software: you can redistribute it and/or modify
 *   it under the terms of the Simplified ("2-clause") BSD licence.
 *
 *   IsoSpec is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  
 *
 *   You should have received a copy of the Simplified BSD Licence
 *   along with IsoSpec.  If not, see <https://opensource.org/licenses/BSD-2-Clause>.
 */


#include <cmath>
#include <algorithm>
#include <vector>
#include <stdlib.h>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <utility>
#include <iostream>
#include <string.h>
#include <limits>
#include "lang.h"
#include "marginalTrek++.h"
#include "conf.h"
#include "allocator.h"
#include "operators.h"
#include "summator.h"
#include "element_tables.h"
#include "misc.h"




Conf initialConfigure(const int atomCnt, const int isotopeNo, const double* probs, const double* lprobs)
{
    Conf res = new int[isotopeNo];

    for(int i = 0; i < isotopeNo; ++i )
    {
        res[i] = int( atomCnt * probs[i] ) + 1;
    }

    int s = 0;

    for(int i = 0; i < isotopeNo; ++i) s += res[i];

    int diff = atomCnt - s;

    // Too little: enlarging fist index.
    if( diff > 0 ){
        res[0] += diff;
    }
    // Too much: trying to redistribute the difference: hopefully the first element is the largest.
    if( diff < 0 ){
        diff = abs(diff);
        int i = 0, coordDiff = 0;

        while( diff > 0){
            coordDiff = res[i] - diff;

            if( coordDiff >= 0 ){
                res[i] -= diff;
                diff = 0;
            } else {
                res[i] = 0;
                i++;
                diff = abs(coordDiff);
            }
        }
    }

    // What we computed so far will be very close to the mode: hillclimb the rest of the way
    
    bool modified = true;
    double LP = logProb(res, lprobs, isotopeNo);
    double NLP;

    while(modified)
    {
    	modified = false;
    	for(int ii = 0; ii<isotopeNo; ii++)
	    for(int jj = 0; jj<isotopeNo; jj++)
	        if(ii != jj and res[ii] > 0)
		{
		    res[ii]--;
		    res[jj]++;
		    NLP = logProb(res, lprobs, isotopeNo);
		    if(NLP>LP)
		    {
		    	modified = true;
			LP = NLP;
		    }
		    else
		    {
		    	res[ii]++;
			res[jj]--;
		    }
		}

	    
    }
    return res;
}



#ifndef BUILDING_R
void printMarginal( const std::tuple<double*,double*,int*,int>& results, int dim)
{
    for(int i=0; i<std::get<3>(results); i++){

        std::cout << "Mass = "  << std::get<0>(results)[i] <<
        " log-prob =\t"                 << std::get<1>(results)[i] <<
        " prob =\t"                     << exp(std::get<1>(results)[i]) <<
        "\tand configuration =\t";

        for(int j=0; j<dim; j++) std::cout << std::get<2>(results)[i*dim + j] << " ";

        std::cout << std::endl;
    }
}
#endif


double* getMLogProbs(const double* probs, int isoNo)
{
    double* ret = new double[isoNo];
    for(int i = 0; i < isoNo; i++)
    {
        ret[i] = log(probs[i]);
        for(int j=0; j<NUMBER_OF_ISOTOPIC_ENTRIES; j++)
            if(elem_table_probability[j] == probs[i])
            {
                ret[i] = elem_table_log_probability[j];
                break;
            }
    }
    return ret;
}


Marginal::Marginal(
    const double* _masses,
    const double* _probs,
    int _isotopeNo,  
    int _atomCnt
) : 
disowned(false),
isotopeNo(_isotopeNo),
atomCnt(_atomCnt),
atom_masses(array_copy<double>(_masses, isotopeNo)),
atom_lProbs(getMLogProbs(_probs, isotopeNo)),
mode_conf(initialConfigure(atomCnt, isotopeNo, _probs, atom_lProbs))
{}

Marginal::Marginal(Marginal&& other) : 
disowned(other.disowned),
isotopeNo(other.isotopeNo),
atomCnt(other.atomCnt),
atom_masses(other.atom_masses),
atom_lProbs(other.atom_lProbs),
mode_conf(other.mode_conf)
{
    other.disowned = true;
}

Marginal::~Marginal()
{
    if(not disowned)
    {
        delete[] atom_masses;
        delete[] atom_lProbs;
        delete[] mode_conf;
    }
}


double Marginal::getLightestConfMass() const
{
    double ret_mass = std::numeric_limits<double>::infinity();
    for(unsigned int ii=0; ii < isotopeNo; ii++)
        if( ret_mass > atom_masses[ii] )
	    ret_mass = atom_masses[ii];
    return ret_mass*atomCnt;
}

double Marginal::getHeaviestConfMass() const
{
    double ret_mass = 0.0;
    for(unsigned int ii=0; ii < isotopeNo; ii++)
        if( ret_mass < atom_masses[ii] )
            ret_mass = atom_masses[ii];
    return ret_mass*atomCnt;
}

double Marginal::getModeLProb() const
{
    return logProb(mode_conf, atom_lProbs, isotopeNo);
}

MarginalTrek::MarginalTrek(
    const double* masses,   // masses size = logProbs size = isotopeNo
    const double* probs,
    int isotopeNo,                  // No of isotope configurations.
    int atomCnt,
    int tabSize,
    int hashSize
) : 
Marginal(masses, probs, isotopeNo, atomCnt),
current_count(0),
keyHasher(isotopeNo),
equalizer(isotopeNo),
orderMarginal(atom_lProbs, isotopeNo),
visited(hashSize,keyHasher,equalizer),
pq(orderMarginal),
totalProb(),
candidate(new int[isotopeNo]),
allocator(isotopeNo, tabSize)
{
    int* initialConf = allocator.makeCopy(mode_conf);

    pq.push(initialConf);
    visited[initialConf] = 0;

    totalProb = Summator();

    current_count = 0;

    add_next_conf();
}


bool MarginalTrek::add_next_conf()
{
    if(pq.size() < 1) return false;

    Conf topConf = pq.top();
    pq.pop();
    ++current_count;
    visited[topConf] = current_count;

    _confs.push_back(topConf);
    _conf_masses.push_back(mass(topConf, atom_masses, isotopeNo));
    double logprob = logProb(topConf, atom_lProbs, isotopeNo);
    _conf_probs.push_back(logprob);


    totalProb.add( exp( logprob ) );

    for( unsigned int i = 0; i < isotopeNo; ++i )
    {
        for( unsigned int j = 0; j < isotopeNo; ++j )
        {
            // Growing index different than decreasing one AND
            // Remain on simplex condition.
            if( i != j && topConf[j] > 0 ){
                copyConf(topConf, candidate, isotopeNo);

                ++candidate[i];
                --candidate[j];

                // candidate should not have been already visited.
                if( visited.count( candidate ) == 0 )
                {
                    Conf acceptedCandidate = allocator.makeCopy(candidate);
                    pq.push(acceptedCandidate);

                    visited[acceptedCandidate] = 0;
                }
            }
        }
    }

    return true;
}

int MarginalTrek::processUntilCutoff(double cutoff)
{
    Summator s;
    int last_idx = -1;
    for(unsigned int i=0; i<_conf_probs.size(); i++)
    {
        s.add(_conf_probs[i]);
        if(s.get() >= cutoff)
        {
            last_idx = i;
            break;
        }
    }
    if(last_idx > -1)
        return last_idx;

    while(totalProb.get() < cutoff && add_next_conf()) {}
    return _conf_probs.size();
}


MarginalTrek::~MarginalTrek()
{
    delete[] candidate;
}




PrecalculatedMarginal::PrecalculatedMarginal(Marginal&& m, 
	double lCutOff,
	bool sort,
        int tabSize,
        int hashSize
) : Marginal(std::move(m)),
allocator(isotopeNo, tabSize)
{
    const ConfEqual equalizer(isotopeNo);
    const KeyHasher keyHasher(isotopeNo);
    const ConfOrderMarginalDescending orderMarginal(atom_lProbs, isotopeNo);

    std::unordered_set<Conf,KeyHasher,ConfEqual> visited(hashSize,keyHasher,equalizer);

    Conf currentConf = allocator.makeCopy(mode_conf);
    if(logProb(currentConf, atom_lProbs, isotopeNo) >= lCutOff)
    {
        configurations.push_back(allocator.makeCopy(currentConf));
        visited.insert(currentConf);
    }

    unsigned int idx = 0;

    while(idx < configurations.size())
    {
        memcpy(currentConf, configurations[idx], sizeof(int)*isotopeNo);
	idx++;
        for(unsigned int ii = 0; ii < isotopeNo; ii++ )
            for(unsigned int jj = 0; jj < isotopeNo; jj++ )
                if( ii != jj and currentConf[jj] > 0)
		{
		    currentConf[ii]++;
		    currentConf[jj]--;

		    if (visited.count(currentConf) == 0 and logProb(currentConf, atom_lProbs, isotopeNo) >= lCutOff)
		    {
		    	 visited.insert(currentConf);
                         configurations.push_back(allocator.makeCopy(currentConf));
	            }

		    currentConf[ii]--;
		    currentConf[jj]++;

                }
    }

    if(sort)
        std::sort(configurations.begin(), configurations.end(), orderMarginal);


    confs  = &configurations[0];
    no_confs = configurations.size();
    lProbs = new double[no_confs];
    eProbs = new double[no_confs];
    masses = new double[no_confs];


    for(unsigned int ii=0; ii < no_confs; ii++)
    {
        lProbs[ii] = logProb(confs[ii], atom_lProbs, isotopeNo);
        eProbs[ii] = exp(lProbs[ii]);
	masses[ii] = mass(confs[ii], atom_masses, isotopeNo);
    }
}


PrecalculatedMarginal::~PrecalculatedMarginal()
{
    if(lProbs != nullptr)
    	delete[] lProbs;
    if(masses != nullptr)
    	delete[] masses;
    if(eProbs != nullptr)
        delete[] eProbs;
}



RGTMarginal::RGTMarginal(
        Marginal&& m,
        double lCutOff,
        int tabSize,
        int hashSize) : 
	PrecalculatedMarginal(std::move(m), lCutOff, true, tabSize, hashSize),
        TOV_NP2(next_pow2(no_confs)),
        TOV_NP2M1(TOV_NP2/2),
        mass_table_rows_no(floor_log2(TOV_NP2M1)),
//        mass_table_row_size(TOV_NP2),
        mass_table_row_size(no_confs),
	mass_table_size(mass_table_row_size * mass_table_rows_no),
	subintervals(alloc_and_setup_subintervals()),
        mass_table(alloc_and_setup_mass_table())
{terminate_search();}



unsigned int* RGTMarginal::alloc_and_setup_subintervals()
{
    unsigned int* ret = new unsigned int[mass_table_size+4];
    unsigned int step = 1;
    unsigned int stepm1 = 0;
    unsigned int counter = 0;
    unsigned int confs_nom1 = no_confs-1;
    TableOrder<double> mass_order(masses);

    for(unsigned int level = 0; level < mass_table_size; level += mass_table_row_size)
    {
        step <<= 1;
        stepm1 = step - 1;
        counter = 0;
        for(unsigned int ii = 0; ii<no_confs; ii++)
        {
            ret[level+ii] = ii;
            if((ii & stepm1) == 0 or ii == confs_nom1)
            {
                std::sort(ret+counter+level, ret+ii+level, mass_order);
                counter = ii;
            }
        }
    }
    return ret;
}




double* RGTMarginal::alloc_and_setup_mass_table()
{
	// Trading off memory for speed here... One less pointer dereference per conf, and better cache coherency. Hopefully. Possibly. 
	double* ret = new double[mass_table_size+4];
        for(unsigned int level = 0; level < mass_table_size; level += mass_table_row_size)
	    for(unsigned int ii=level; ii<no_confs+level; ii++)
		ret[ii] = masses[subintervals[ii]];

	return ret;
}

/*
 * -------- end of constructor -------------
 */


void RGTMarginal::setup_search(double _pmin, double _pmax, double _mmin, double _mmax)
{
    pmin = _pmin;
    pmax = _pmax;
    mmin = _mmin;
    mmax = _mmax;
    mask = ~1;
    gap = 2;
    arridx = mass_table_size;
    arrend = mass_table_size;



    if(pmax >= 0.0)
        lower = 0;
    else
    {
        lower = std::lower_bound(lProbs, lProbs+no_confs, pmax, rev_ord)-lProbs;
        if(lower == no_confs)
        {
            terminate_search();
            return;
        }
    }

    if(std::isinf(pmin) and std::signbit(pmin))
        upper = no_confs-1;
    else
    {
        upper = (std::upper_bound(lProbs, lProbs+no_confs, pmin, rev_ord)-lProbs);
        if(upper == 0)
        {
            terminate_search();
            return;
        }
        upper--;
    }


    if(lower > upper)
    {   
        terminate_search();
        return;
    }
    
    if(mmin <= masses[lower] and masses[lower] <= mmax)
    {
        subintervals[arrend] = lower;
        mass_table[arrend] = masses[lower];
        arrend++;
    }

    if(upper == lower)
        return;

    if(mmin <= masses[upper] and masses[upper] <= mmax and lProbs[upper] >= pmin)
    {
        subintervals[arrend] = upper;
        mass_table[arrend] = masses[upper];
        arrend++;
    }

    if((upper & ~1) == lower)
    {
        return;
    }

    if((lower & 1) == 0)
    {
        // Is a left child: add right child too
        lower++;
        if(lower == upper)
            return;
        
        if(mmin <= masses[lower] and masses[lower] <= mmax)
        {
            subintervals[arrend] = lower;
            mass_table[arrend] = masses[lower];
            arrend++;
        }
    }

    if((upper & 1) == 1)
    {
        // Is a right child: add left child
        upper--;

        if(mmin <= masses[upper] and masses[upper] <= mmax and lProbs[upper] >= pmin)
        {
            subintervals[arrend] = upper;
            mass_table[arrend] = masses[upper];
            arrend++;
        }
    }

    for(unsigned int ii=arridx; ii<arrend; ii++)
        mass_table[ii] = masses[subintervals[ii]];

    lower &= mask;
    upper &= mask;

    current_level = -mass_table_row_size;
    going_up = true;

}


bool RGTMarginal::next()
{
    // TODO: move to .h and inline this.
    if(arridx < arrend and mass_table[arridx] <= mmax)
    {
        cidx = subintervals[arridx];
        arridx++;
        return true;
    }

    return hard_next();
}
#include <bitset>

bool RGTMarginal::hard_next()
{
    if(upper == lower or (upper & mask) == lower)
    {
        terminate_search();
        return false;
    }
    unsigned int nextmask = mask << 1;
    if(going_up)
    {
        going_up = false;
        current_level += mass_table_row_size;
        gap <<= 1;
        if((upper & (nextmask)) == (lower & (nextmask)))
        {
            terminate_search();
            return false;
        }
        if((upper & ~nextmask) != 0)
        {
            // Coming from right child
            // Add left sibling
            arrend = upper+current_level;
            upper &= nextmask; 
            unsigned int dstart = current_level+upper;
            unsigned int dend   = arrend;
            arridx = std::lower_bound(mass_table+dstart, mass_table+dend, mmin) - mass_table;
            return next();
        }
        else
        {
            // Coming from left child
            // Do nothing
            upper &= nextmask;
            return hard_next();
        }
    }
    else 
    {
        going_up = true;
        if((lower & ~nextmask) != 0)
        {
            // Coming from right child
            // Do nothing
            lower &= nextmask;
            mask <<= 1;
            return hard_next();
        }
        else
        {
        unsigned int search_start = lower+(gap/2);
            arrend = lower + current_level + gap;
            arridx = std::lower_bound(mass_table+current_level+search_start, mass_table+arrend, mmin) - mass_table;
            lower &= nextmask;
            mask <<= 1;
            return next();
        }

    }

}

#define INF std::numeric_limits<double>::infinity()
#define MIN(a, b) std::min<double>(a, b)

double RGTMarginal::min_mass_above_lProb(double prob)
{
    // TODO: precalculate 'em bastards!
    setup_search(prob, INF, -INF, INF);
    double acc = INF;
    for(;arridx < arrend; arridx++)
        acc = MIN(acc, mass_table[arridx]);
    while(next())
    {
        acc = MIN(acc, mass_table[arridx]);
        arridx = arrend;
    }
    return acc;
}

#define MAX(a, b) std::max<double>(a, b)
double RGTMarginal::max_mass_above_lProb(double prob)
{
    // TODO: same as above.
    setup_search(prob, INF, -INF, INF);
    double acc = -INF;
    for(;arridx < arrend; arridx++)
        acc = MAX(acc, mass_table[arridx]);
    while(next())
    {
        acc = MAX(acc, mass_table[arrend-1]);
        arridx = arrend;
    }
    return acc;
}


void RGTMarginal::terminate_search()
{
arridx = arrend = lower = upper = 0;
pmin = pmax = mmin = mmax = 0.0;
}

RGTMarginal::~RGTMarginal()
{
    delete[] mass_table;
    delete[] subintervals;
}

