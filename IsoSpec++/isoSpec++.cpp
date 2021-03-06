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
#include <string.h>
#include <tuple>
#include <unordered_map>
#include <queue>
#include <utility>
#include <iostream>
#include <iomanip>
#include <cctype>
#include <stdexcept>
#include <string>
#include <limits>
#include <assert.h>
#include "lang.h"
#include "conf.h"
#include "dirtyAllocator.h"
#include "operators.h"
#include "summator.h"
#include "marginalTrek++.h"
#include "isoSpec++.h"
#include "misc.h"
#include "element_tables.h"


using namespace std;

Iso::Iso(
    int             _dimNumber,
    const int*      _isotopeNumbers,
    const int*      _atomCounts,
    const double**  _isotopeMasses,
    const double**  _isotopeProbabilities
) :
disowned(false),
dimNumber(_dimNumber),
isotopeNumbers(array_copy<int>(_isotopeNumbers, _dimNumber)),
atomCounts(array_copy<int>(_atomCounts, _dimNumber)),
confSize(_dimNumber * sizeof(int)),
allDim(0),
marginals(nullptr),
modeLProb(0.0)
{
	setupMarginals(_isotopeMasses, _isotopeProbabilities);
}

Iso::Iso(Iso&& other) :
disowned(other.disowned),
dimNumber(other.dimNumber),
isotopeNumbers(other.isotopeNumbers),
atomCounts(other.atomCounts),
confSize(other.confSize),
allDim(other.allDim),
marginals(other.marginals),
modeLProb(other.modeLProb)
{
    other.disowned = true;
}


Iso::Iso(const Iso& other, bool fullcopy) : 
disowned(fullcopy ? throw std::logic_error("Not implemented") : true),
dimNumber(other.dimNumber),
isotopeNumbers(fullcopy ? array_copy<int>(other.isotopeNumbers, dimNumber) : other.isotopeNumbers),
atomCounts(fullcopy ? array_copy<int>(other.atomCounts, dimNumber) : other.atomCounts),
confSize(other.confSize),
allDim(other.allDim),
marginals(fullcopy ? throw std::logic_error("Not implemented") : other.marginals),
modeLProb(other.modeLProb)
{}


inline void Iso::setupMarginals(const double** _isotopeMasses, const double** _isotopeProbabilities)
{
    if (marginals == nullptr)
    {
        marginals = new Marginal*[dimNumber];
        for(int i=0; i<dimNumber;i++) 
        {
	    allDim += isotopeNumbers[i];
	    marginals[i] = new Marginal(
                _isotopeMasses[i],
                _isotopeProbabilities[i],
                isotopeNumbers[i],
                atomCounts[i]
             );
             modeLProb += marginals[i]->getModeLProb();
        }
    }

}

Iso::~Iso()
{
    if(not disowned)
    {
	if (marginals != nullptr)
	    dealloc_table(marginals, dimNumber);
	delete[] isotopeNumbers;
	delete[] atomCounts;
    }
}


double Iso::getLightestPeakMass() const
{
    double mass = 0.0;
    for (int ii=0; ii<dimNumber; ii++)
        mass += marginals[ii]->getLightestConfMass();
    return mass;
}

double Iso::getHeaviestPeakMass() const
{
    double mass = 0.0;
    for (int ii=0; ii<dimNumber; ii++)
        mass += marginals[ii]->getHeaviestConfMass();
    return mass;
}



IsoSpec::IsoSpec(
    int             _dimNumber,
    const int*      _isotopeNumbers,
    const int*      _atomCounts,
    const double**  isotopeMasses,
    const double**  isotopeProbabilities,
    const double    _cutOff,
    int             tabSize
) : Iso(_dimNumber, _isotopeNumbers, _atomCounts, isotopeMasses, isotopeProbabilities),
cutOff(_cutOff),
allocator(_dimNumber, tabSize),
cnt(0),
candidate(new int[dimNumber]),
allDim(0)
{
    logProbs        = new const vector<double>*[dimNumber];
    masses          = new const vector<double>*[dimNumber];
    marginalConfs   = new const vector<int*>*[dimNumber];

    for(int i = 0; i<dimNumber; i++)
    {
        masses[i] = &marginalResults[i]->conf_masses();
        logProbs[i] = &marginalResults[i]->conf_probs();
        marginalConfs[i] = &marginalResults[i]->confs();
    }

    initialConf     = allocator.newConf();
    memset(
        reinterpret_cast<char*>(initialConf) + sizeof(double),
           0,
           sizeof(int)*dimNumber
    );

    *(reinterpret_cast<double*>(initialConf)) =
    combinedSum(
        getConf(initialConf),
                logProbs,
                dimNumber
    );

}


inline int str_to_int(const string& s)
{
	char* endptr[1];
	const char* c_s = s.c_str();
	int ret = (int) strtol(c_s, endptr, 10);
	if (c_s == endptr[0])
		throw invalid_argument("Invalid formula");
	return ret;
}

Iso::Iso(const char* formula) :
disowned(false),
marginals(nullptr),
modeLProb(0.0)
{
std::vector<const double*> isotope_masses;
std::vector<const double*> isotope_probabilities;

dimNumber = parse_formula(formula, isotope_masses, isotope_probabilities, &isotopeNumbers, &atomCounts, &confSize);

setupMarginals(isotope_masses.data(), isotope_probabilities.data());
}

unsigned int parse_formula(const char* formula, std::vector<const double*>& isotope_masses, std::vector<const double*>& isotope_probabilities, int** isotopeNumbers, int** atomCounts, unsigned int* confSize)
{
// This function is NOT guaranteed to be secure againt malicious input. It should be used only for debugging.

    string cpp_formula(formula);
    int last_modeswitch = 0;
    int mode = 0;
    int pos = 0;
    std::vector<string> elements;
    std::vector<int> numbers;
    while(formula[pos] != '\0')
    {
        if(isdigit(formula[pos]) && mode == 0)
        {
            elements.push_back(cpp_formula.substr(last_modeswitch, pos-last_modeswitch));
            last_modeswitch = pos;
            mode = 1;
        }
        else if(isalpha(formula[pos]) && mode == 1)
        {
            numbers.push_back(str_to_int(cpp_formula.substr(last_modeswitch, pos-last_modeswitch)));
            last_modeswitch = pos;
            mode = 0;
        }
        pos++;
    }

    numbers.push_back(str_to_int(cpp_formula.substr(last_modeswitch, pos)));


    if(elements.size() != numbers.size())
        throw invalid_argument("Invalid formula");

    std::vector<int> element_indexes;

    for (unsigned int i=0; i<elements.size(); i++)
    {
        int idx = -1;
        for(int j=0; j<NUMBER_OF_ISOTOPIC_ENTRIES; j++)
        {
            if (elements[i].compare(elem_table_symbol[j]) == 0)
            {
                idx = j;
                break;
            }
        }
        if(idx < 0)
            throw invalid_argument("Invalid formula");
        element_indexes.push_back(idx);

    }

    vector<int> _isotope_numbers;

    for(vector<int>::iterator it = element_indexes.begin(); it != element_indexes.end(); ++it)
    {
        int num = 0;
        int at_idx = *it;
        int atomicNo = elem_table_atomicNo[at_idx];
        while(at_idx < NUMBER_OF_ISOTOPIC_ENTRIES && elem_table_atomicNo[at_idx] == atomicNo)
        {
            at_idx++;
            num++;
        }
        _isotope_numbers.push_back(num);
    }

    for(vector<int>::iterator it = element_indexes.begin(); it != element_indexes.end(); ++it)
    {
        isotope_masses.push_back(&elem_table_mass[*it]);
        isotope_probabilities.push_back(&elem_table_probability[*it]);
    };

    const unsigned int dimNumber = elements.size();

    *isotopeNumbers = array_copy<int>(_isotope_numbers.data(), dimNumber);
    *atomCounts = array_copy<int>(numbers.data(), dimNumber);
    *confSize = dimNumber * sizeof(int);

    return dimNumber;

}


void IsoSpec::processConfigurationsUntilCutoff()
{
    while( cutOff > totalProb.get() && advanceToNextConfiguration() ) {}
}



std::tuple<double*,double*,int*,int> IsoSpec::getProduct()
{
    processConfigurationsUntilCutoff();
    return getCurrentProduct();
}


std::tuple<double*,double*,int*,int> IsoSpec::getCurrentProduct()
{

    double*         res_mass        = new double[cnt];
    double*         res_logProb     = new double[cnt];
    int*            res_isoCounts   = new int[cnt*allDim];

    getCurrentProduct(res_mass, res_logProb, res_isoCounts);

    return std::tuple<double*,double*,int*,int>(
        res_mass,
        res_logProb,
        res_isoCounts,
        cnt
    );
}


void IsoSpec::getProduct(double* res_mass, double* res_logProb, int* res_isoCounts)
{
    processConfigurationsUntilCutoff();
    getCurrentProduct(res_mass, res_logProb, res_isoCounts);
}


void IsoSpec::getCurrentProduct(double* res_mass, double* res_logProb, int* res_isoCounts)
{

    int i = 0;
    int j = 0;

    for(unsigned int na_idx = 0; na_idx < newaccepted.size(); na_idx++)
    {
        int* curr_conf  = getConf(newaccepted[na_idx]);

	if(res_mass != NULL)
        	res_mass[i]     = combinedSum( curr_conf, masses, dimNumber );

	if(res_logProb != NULL)
        	res_logProb[i]  = getLProb(newaccepted[na_idx]);

	if(res_isoCounts != NULL)
	{
            for(int isotopeNumber=0; isotopeNumber<dimNumber; isotopeNumber++)
            {
                int currentConfIndex = curr_conf[isotopeNumber];
                int locIsoNo = isotopeNumbers[isotopeNumber];
                memcpy(
                    &res_isoCounts[j],
                    (*marginalConfs[isotopeNumber])[currentConfIndex],
                        sizeof(int)*locIsoNo
                );
                j += locIsoNo;
            }
	}
        i++;
    }
}

int IsoSpec::getNoVisitedConfs()
{
    return newaccepted.size();
}

int IsoSpec::getNoIsotopesTotal()
{
    int ret = 0;
    for(int i=0; i<dimNumber; i++)
        ret += isotopeNumbers[i];
    return ret;
}

IsoSpec::~IsoSpec()
{
    delete[] candidate;
    delete[] logProbs;
    delete[] masses;
    delete[] marginalConfs;
}



IsoSpecLayered::IsoSpecLayered( int             _dimNumber,
                                const int*      _isotopeNumbers,
                                const int*      _atomCounts,
                                const double**  _isotopeMasses,
                                const double**  _isotopeProbabilities,
                                const double    _cutOff,
                                int             tabSize,
                                double          layerStep,
                                bool            _estimateThresholds,
				bool            trim
) : IsoSpec( _dimNumber,
             _isotopeNumbers,
             _atomCounts,
             _isotopeMasses,
             _isotopeProbabilities,
             _cutOff,
             tabSize = 1000
),
estimateThresholds(_estimateThresholds),
do_trim(trim),
layers(0)
{
    current = new std::vector<void*>();
    next    = new std::vector<void*>();

    current->push_back(initialConf);

    percentageToExpand = layerStep;
    lprobThr = (*reinterpret_cast<double*>(initialConf));
}


IsoSpecLayered::~IsoSpecLayered()
{
    if(current != NULL)
        delete current;
    if(next != NULL)
        delete next;
}

bool IsoSpecLayered::advanceToNextConfiguration()
{
    layers += 1;
    double maxFringeLprob = -std::numeric_limits<double>::infinity();

    if(current == NULL)
        return false;
    int accepted_in_this_layer = 0;
    Summator prob_in_this_layer(totalProb);


    while(current->size() > 0)
    {
        topConf = current->back();
        current->pop_back();

        cnt++;

        double top_lprob = getLProb(topConf);

        if(top_lprob >= lprobThr)
        {
#ifdef DEBUG
            hits += 1;
#endif /* DEBUG */
            newaccepted.push_back(topConf);
            accepted_in_this_layer++;
            prob_in_this_layer.add(exp(top_lprob));
        }
        else
        {
#ifdef DEBUG
            moves += 1;
#endif /* DEBUG */
            next->push_back(topConf);
            continue;
        }

        int* topConfIsoCounts = getConf(topConf);

        for(int j = 0; j < dimNumber; ++j)
        {
            // candidate cannot refer to a position that is
            // out of range of the stored marginal distribution.
            if(marginalResults[j]->probeConfigurationIdx(topConfIsoCounts[j] + 1))
            {
                memcpy(candidate, topConfIsoCounts, confSize);
                candidate[j]++;

                void*       acceptedCandidate          = allocator.newConf();
                int*        acceptedCandidateIsoCounts = getConf(acceptedCandidate);
                memcpy(     acceptedCandidateIsoCounts, candidate, confSize);

                double newConfProb = combinedSum(
                    candidate,
                    logProbs,
                    dimNumber
                );



                *(reinterpret_cast<double*>(acceptedCandidate)) = newConfProb;

                if(newConfProb >= lprobThr)
                    current->push_back(acceptedCandidate);
                else
        {
                    next->push_back(acceptedCandidate);
            if(newConfProb > maxFringeLprob)
                maxFringeLprob = top_lprob;
        }
            }
            if(topConfIsoCounts[j] > 0)
                break;
        }
    }

    if(next == NULL || next->size() < 1)
        return false;
    else
    {
        if(prob_in_this_layer.get() < cutOff)
        {
#ifdef DEBUG
            Summator testDupa(prob_in_this_layer);
            for (std::vector<void*>::iterator it = next->begin(); it != next->end(); it++) {
                testDupa.add(exp(getLProb(*it)));
            }
            std::cout << "Prob(Layer) = " << prob_in_this_layer.get() << std::endl;
            std::cout << "Prob(Layer)+Prob(Fringe) = " << testDupa.get() << std::endl;
            std::cout << "Layers = " << layers << std::endl;
            std::cout << std::endl;
#endif /* DEBUG */

        // // This was an attempt to merge two methods: layered and layered_estimating
        // // that does not work so good as predicted.
//             if( estimateThresholds and ( prob_in_this_layer.get() >= cutOff*.99 ) ){
//                 estimateThresholds = false;
//                 percentageToExpand = .25; // The ratio of one rectangle to the rectangle.
// #ifdef DEBUG
//                 std::cout << "We switch!" << std::endl;
// #endif /* DEBUG */
//             }

#ifdef DEBUG
                std::cout << "percentageToExpand = " << percentageToExpand << std::endl;
#endif /* DEBUG */

            std::vector<void*>* nnew = current;
            nnew->clear();
            current = next;
            next = nnew;
            int howmany = floor(current->size()*percentageToExpand);
            if(estimateThresholds){
                // Screw numeric correctness, ARRRRRRR!!! Well, this is an estimate anyway, doesn't have to be that precise
                // lprobThr += log((1.0-cutOff))+log1p((percentageToExpand-1.0)/layers) - log(1.0-prob_in_this_layer.get());
                lprobThr += log(1.0-cutOff) + log(1.0-(1.0-percentageToExpand)/pow(layers, 2.0)) - log(1.0 -prob_in_this_layer.get());
                if(lprobThr > maxFringeLprob){
                    lprobThr = maxFringeLprob;
                    estimateThresholds = false;
                    percentageToExpand = .3;
#ifdef DEBUG
                    std::cout << "We switch to other method because density estimates where higher than max on fringe." << std::endl;
#endif /* DEBUG */
                    lprobThr = getLProb(quickselect(current->data(), howmany, 0, current->size()));
                }
            } else
                lprobThr = getLProb(quickselect(current->data(), howmany, 0, current->size()));
            totalProb = prob_in_this_layer;
        }
        else
        {
#ifdef DEBUG
            std::cerr << "No. layers: " << layers << "  hits: " << hits << "    misses: " << moves << " miss ratio: " << static_cast<double>(moves) / static_cast<double>(hits) << std::endl;
#endif /* DEBUG */
            delete next;
            next = NULL;
            delete current;
            current = NULL;
            int start = 0;
            int end = accepted_in_this_layer - 1;
            void* swapspace;

            if(do_trim)
            {
                void** lastLayer = &(newaccepted.data()[newaccepted.size()-accepted_in_this_layer]);

                Summator qsprob(totalProb);
                while(totalProb.get() < cutOff)
                {
                    if(start == end)
                        break;

                    // Partition part

                    int len = end - start;
#ifdef BUILDING_R
            int pivot = len/2 + start;  // We're very definitely NOT switching to R to use a RNG, and if R sees us use C RNG it complains...
#else
            int pivot = rand() % len + start;
#endif
                    void* pval = lastLayer[pivot];
                    double pprob = getLProb(pval);
                    mswap(lastLayer[pivot], lastLayer[end-1]);
                    int loweridx = start;
                    for(int i=start; i<end-1; i++)
                    {
                        if(getLProb(lastLayer[i]) > pprob)
                        {
                            mswap(lastLayer[i], lastLayer[loweridx]);
                            loweridx++;
                        }
                    }
                    mswap(lastLayer[end-1], lastLayer[loweridx]);

                    // Selection part

                    Summator leftProb(qsprob);
                    for(int i=start; i<=loweridx; i++)
                    {
                        leftProb.add(exp(getLProb(lastLayer[i])));
                    }
                    if(leftProb.get() < cutOff)
                    {
                        start = loweridx+1;
                        qsprob = leftProb;
                    }
                    else
                        end = loweridx;
                }
            int accend = newaccepted.size()-accepted_in_this_layer+start+1;
    #ifdef DEBUG
                std::cerr << "Last layer size: " << accepted_in_this_layer << " Total size: " << newaccepted.size() << "    Total size after trimming: " << accend << " No. trimmed: " << -start-1+accepted_in_this_layer
            << "    Trimmed to left ratio: " << static_cast<double>(-start-1+accepted_in_this_layer) / static_cast<double>(accend) << std::endl;
    #endif /* DEBUG */

                totalProb = qsprob;
                newaccepted.resize(accend);
                return true;
            }
            else // No trimming
            {
                totalProb = prob_in_this_layer;
                return true;
            }
        }
    }
    return true;

}




/*
 * ----------------------------------------------------------------------------------------------------------
 */



IsoGenerator::IsoGenerator(Iso&& iso) : 
    Iso(std::move(iso)), 
    partialLProbs(new double[dimNumber+1+PADDING]), 
    partialMasses(new double[dimNumber+1+PADDING]),
    partialExpProbs(new double[dimNumber+1+PADDING])
{
    partialLProbs[dimNumber] = 0.0;
    partialMasses[dimNumber] = 0.0;
    partialExpProbs[dimNumber] = 1.0;
}





IsoThresholdGeneratorBoundMass::IsoThresholdGeneratorBoundMass(Iso&& iso, double _threshold, double _min_mass, double _max_mass, bool _absolute, int tabSize, int hashSize)
: IsoGenerator(std::move(iso)),
min_mass(_min_mass),
max_mass(_max_mass)
{
	maxConfsLPSum 	= new double[dimNumber];
        minMassCSum     = new double[dimNumber];
        maxMassCSum     = new double[dimNumber];

        marginalResults = new RGTMarginal*[dimNumber];

        Lcutoff = log(_threshold);
        if(not _absolute)
            Lcutoff += modeLProb;

        bool empty = false;
	for(int ii=0; ii<dimNumber; ii++)
	{
            marginalResults[ii] = new RGTMarginal(std::move(*(marginals[ii])), 
                                                            Lcutoff - modeLProb + marginals[ii]->getModeLProb(),
                                                            tabSize, 
                                                            hashSize);

            if(not marginalResults[ii]->inRange(0))
                empty = true;
	}
	maxConfsLPSum[0] = marginalResults[0]->getModeLProb();
        minMassCSum[0] = marginalResults[0]->getLightestConfMass();
        maxMassCSum[0] = marginalResults[0]->getHeaviestConfMass();

	for(int ii=1; ii<dimNumber; ii++)
        {
	    maxConfsLPSum[ii] = maxConfsLPSum[ii-1] + marginalResults[ii]->getModeLProb();
            minMassCSum[ii] = minMassCSum[ii-1] + marginalResults[ii]->getLightestConfMass();
            maxMassCSum[ii] = maxMassCSum[ii-1] + marginalResults[ii]->getHeaviestConfMass();
        }


        setup_ith_marginal_range(dimNumber-1);

        for(int ii=0; ii<dimNumber-1; ii++)
            marginalResults[0]->setup_search(2.0, 1.0, std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity());

}

IsoThresholdGeneratorBoundMass::~IsoThresholdGeneratorBoundMass() 
{ 
    delete[] maxConfsLPSum;
    dealloc_table(marginalResults, dimNumber);
    delete[] minMassCSum;
    delete[] maxMassCSum;
}

bool IsoThresholdGeneratorBoundMass::advanceToNextConfiguration()
{
	if(marginalResults[0]->next())
        {
	    recalc(0);
            return true;
        }
            

	// If we reached this point, a carry is needed
	
	int idx = 1;
        bool frombelow = true;


        while(idx >= 0 and idx < dimNumber)
        {
            if(frombelow)
            {
                if(marginalResults[idx]->next())
                {
                    recalc(idx);
                    frombelow = false;
                    idx--;
                }
                else
                    idx++;
            }
            else
            {
                setup_ith_marginal_range(idx);
                if(marginalResults[idx]->next())
                {
                    recalc(idx);
                    idx--;
                }
                else
                {
                    idx++;
                    frombelow = true;
                }
            }
        }
                
        if(idx == dimNumber)
            return false;
        else
            return true;

        
}


void IsoThresholdGeneratorBoundMass::setup_ith_marginal_range(unsigned int idx)
{
    double lower_min = min_mass - partialMasses[idx+1];
    double lower_max = max_mass - partialMasses[idx+1];
    double rem_prob  = Lcutoff - partialLProbs[idx+1] - maxConfsLPSum[idx];

    for(unsigned int ii=0; ii < idx; ii++)
    {
        double p = rem_prob + marginalResults[ii]->getModeLProb();
        lower_min -= marginalResults[ii]->max_mass_above_lProb(p);
        lower_max -= marginalResults[ii]->min_mass_above_lProb(p);
    }


    marginalResults[idx]->setup_search(rem_prob+marginalResults[idx]->getModeLProb(), std::numeric_limits<double>::infinity(), lower_min, lower_max);
    if(idx>2){
    unsigned int cou = 0;
    while(marginalResults[idx]->next())
        cou++;
    }
    marginalResults[idx]->setup_search(rem_prob+marginalResults[idx]->getModeLProb(), std::numeric_limits<double>::infinity(), lower_min, lower_max);

}

/*
 * ----------------------------------------------------------------------------------------------------------
 */




PrecalculatedMarginal** Iso::get_MT_marginal_set(double Lcutoff, bool absolute, int tabSize, int hashSize)
{
    PrecalculatedMarginal** ret = new PrecalculatedMarginal*[dimNumber];

    if(absolute)
        Lcutoff -= modeLProb;

    for(int ii = 0; ii<dimNumber - 1; ii++)
        ret[ii] = new PrecalculatedMarginal(std::move(*(marginals[ii])),
                                            Lcutoff + marginals[ii]->getModeLProb(),
                                            true,
                                            tabSize,
                                            hashSize);


    const unsigned int ii = dimNumber - 1;
    ret[ii] = new SyncMarginal(std::move(*(marginals[ii])),
                            Lcutoff + marginals[ii]->getModeLProb(),
                            tabSize,
                            hashSize);
    return ret;
}


IsoThresholdGeneratorMT::IsoThresholdGeneratorMT(Iso&& iso, double _threshold, PrecalculatedMarginal** PMs, bool _absolute)
: IsoGenerator(Iso(iso, false)),
Lcutoff(_absolute ? log(_threshold) : log(_threshold) + modeLProb),
last_marginal(static_cast<SyncMarginal*>(PMs[dimNumber-1]))
{
	counter 	= new unsigned int[dimNumber+PADDING];
	maxConfsLPSum 	= new double[dimNumber-1];

        marginalResults = PMs;

        bool empty = false;
	for(int ii=0; ii<dimNumber-1; ii++)
	{
	    counter[ii] = 0;

            if(not marginalResults[ii]->inRange(0))
                empty = true;
	}

        marginalResults[dimNumber-1] = last_marginal;
        counter[dimNumber-1] = last_marginal->getNextConfIdx();
        if(not last_marginal->inRange(counter[dimNumber-1]))
            empty = true;


	maxConfsLPSum[0] = marginalResults[0]->getModeLProb();
	for(int ii=1; ii<dimNumber-1; ii++)
	    maxConfsLPSum[ii] = maxConfsLPSum[ii-1] + marginalResults[ii]->getModeLProb();


        if(not empty)
        {
            recalc(dimNumber-1);
            counter[0]--;
        }
        else 
            terminate_search();
}

bool IsoThresholdGeneratorMT::advanceToNextConfiguration()
{
	counter[0]++;
	if(marginalResults[0]->inRange(counter[0]))
	{
		partialLProbs[0] = partialLProbs[1] + marginalResults[0]->get_lProb(counter[0]);
		if(partialLProbs[0] >= Lcutoff)
		{
			partialMasses[0] = partialMasses[1] + marginalResults[0]->get_mass(counter[0]);
                        partialExpProbs[0] = partialExpProbs[1] * marginalResults[0]->get_eProb(counter[0]);
			return true;
		}
	}

	// If we reached this point, a carry is needed
	
	int idx = 0;

	while(idx<dimNumber-2)
	{
		counter[idx] = 0;
		idx++;
		counter[idx]++;
		if(marginalResults[idx]->inRange(counter[idx]))
		{
			partialLProbs[idx] = partialLProbs[idx+1] + marginalResults[idx]->get_lProb(counter[idx]);
			if(partialLProbs[idx] + maxConfsLPSum[idx-1] >= Lcutoff)
			{
				partialMasses[idx] = partialMasses[idx+1] + marginalResults[idx]->get_mass(counter[idx]);
                                partialExpProbs[idx] = partialExpProbs[idx+1] * marginalResults[idx]->get_eProb(counter[idx]);
				recalc(idx-1);
				return true;
			}
		}
	}

        counter[idx] = 0;
        idx++;
        counter[idx] = last_marginal->getNextConfIdx();
        if(last_marginal->inRange(counter[idx]))
        {
            partialLProbs[idx] = partialLProbs[idx+1] + last_marginal->get_lProb(counter[idx]);
            if(partialLProbs[idx] + maxConfsLPSum[idx-1] >= Lcutoff)
            {
                partialMasses[idx] = partialMasses[idx+1] + last_marginal->get_mass(counter[idx]);
                partialExpProbs[idx] = partialExpProbs[idx+1] * last_marginal->get_eProb(counter[idx]);
                recalc(idx-1);
                return true;
            }
        }
        terminate_search();
	return false;
}

void IsoThresholdGeneratorMT::terminate_search()
{
    for(int ii=0; ii<dimNumber; ii++)
        counter[ii] = marginalResults[ii]->get_no_confs();
}

/*
 * ----------------------------------------------------------------------------------------------------------
 */










IsoThresholdGenerator::IsoThresholdGenerator(Iso&& iso, double _threshold, bool _absolute, int tabSize, int hashSize)
: IsoGenerator(std::move(iso)),
Lcutoff(_absolute ? log(_threshold) : log(_threshold) + modeLProb)
{
	counter 	= new unsigned int[dimNumber];
	maxConfsLPSum 	= new double[dimNumber-1];

        marginalResults = new PrecalculatedMarginal*[dimNumber];

        bool empty = false;
	for(int ii=0; ii<dimNumber; ii++)
	{
	    counter[ii] = 0;

            marginalResults[ii] = new PrecalculatedMarginal(std::move(*(marginals[ii])), 
                                                            Lcutoff - modeLProb + marginals[ii]->getModeLProb(),
                                                            true,
                                                            tabSize, 
                                                            hashSize);

            if(not marginalResults[ii]->inRange(0))
                empty = true;
	}

	maxConfsLPSum[0] = marginalResults[0]->getModeLProb();
	for(int ii=1; ii<dimNumber-1; ii++)
	    maxConfsLPSum[ii] = maxConfsLPSum[ii-1] + marginalResults[ii]->getModeLProb();

        if(not empty)
        {
            recalc(dimNumber-1);
            counter[0]--;
        }
        else
            terminate_search();
            

}

bool IsoThresholdGenerator::advanceToNextConfiguration()
{
	counter[0]++;
	if(marginalResults[0]->inRange(counter[0]))
	{
		partialLProbs[0] = partialLProbs[1] + marginalResults[0]->get_lProb(counter[0]);
		if(partialLProbs[0] >= Lcutoff)
		{
			partialMasses[0] = partialMasses[1] + marginalResults[0]->get_mass(counter[0]);
                        partialExpProbs[0] = partialExpProbs[1] * marginalResults[0]->get_eProb(counter[0]);
			return true;
		}
	}

	// If we reached this point, a carry is needed
	
	int idx = 0;

	while(idx<dimNumber-1)
	{
		counter[idx] = 0;
		idx++;
		counter[idx]++;
		if(marginalResults[idx]->inRange(counter[idx]))
		{
			partialLProbs[idx] = partialLProbs[idx+1] + marginalResults[idx]->get_lProb(counter[idx]);
			if(partialLProbs[idx] + maxConfsLPSum[idx-1] >= Lcutoff)
			{
				partialMasses[idx] = partialMasses[idx+1] + marginalResults[idx]->get_mass(counter[idx]);
                                partialExpProbs[idx] = partialExpProbs[idx+1] * marginalResults[idx]->get_eProb(counter[idx]);
				recalc(idx-1);
				return true;
			}
		}
	}

        terminate_search();
	return false;
}

void IsoThresholdGenerator::terminate_search()
{
    for(int ii=0; ii<dimNumber; ii++)
        counter[ii] = marginalResults[ii]->get_no_confs();
}

/*
 * ----------------------------------------------------------------------------------------------------------
 */






/*
 * ------------------------------------------------------------------------------------------------------------------------
 */
#if 0

void IsoOrderedGenerator::IsoOrderedGenerator_init(const double    _cutOff)
{
    logProbs        = new const vector<double>*[dimNumber];
    masses          = new const vector<double>*[dimNumber];
    marginalConfs   = new const vector<int*>*[dimNumber];
    candidate	    = new int[dimNumber];

    for(int i = 0; i<dimNumber; i++)
    {
        masses[i] = &marginalResults[i]->conf_masses();
        logProbs[i] = &marginalResults[i]->conf_probs();
        marginalConfs[i] = &marginalResults[i]->confs();
    }

    topConf     = allocator.newConf();
    memset(
        reinterpret_cast<char*>(topConf) + sizeof(double),
           0,
           sizeof(int)*dimNumber
    );

    *(reinterpret_cast<double*>(topConf)) =
    combinedSum(
        getConf(topConf),
                logProbs,
                dimNumber
    );

    pq.push(topConf);
    cutOff = _cutOff;

}


IsoOrderedGenerator::~IsoOrderedGenerator(){}

bool IsoOrderedGenerator::advanceToNextConfiguration()
{
    if(pq.size() < 1)
        return false;


    topConf = pq.top();
    pq.pop();

    int* topConfIsoCounts = getConf(topConf);

    // newaccepted.push_back(topConf);
    currentLProb = *(reinterpret_cast<double*>(topConf));
    currentMass = combinedSum( topConfIsoCounts, masses, dimNumber );

    // totalProb.add( exp(*reinterpret_cast<double*>(topConf) ));

    for(int j = 0; j < dimNumber; ++j)
    {
        // candidate cannot refer to a position that is
        // out of range of the stored marginal distribution.
        if(marginalResults[j]->probeConfigurationIdx(topConfIsoCounts[j] + 1))
        {
            memcpy(candidate, topConfIsoCounts, confSize);
            candidate[j]++;

            double candidateLProb = combinedSum(candidate, logProbs, dimNumber);

            if (candidateLProb > cutOff)
	    {
                void*       acceptedCandidate                       = allocator.newConf();
                int*        acceptedCandidateIsoCounts      = getConf(acceptedCandidate);
                memcpy(     acceptedCandidateIsoCounts, candidate, confSize);

                *(reinterpret_cast<double*>(acceptedCandidate)) = candidateLProb;
                pq.push(acceptedCandidate);
	    }
        }
        if(topConfIsoCounts[j] > 0)
            break;
    }


    return true;
}
#endif
#ifndef BUILDING_R

void printConfigurations(
    const   std::tuple<double*,double*,int*,int>& results,
    int     dimNumber,
    int*    isotopeNumbers
){
    int m = 0;

    for(int i=0; i<std::get<3>(results); i++){

        std::cout << "Mass = "  << std::get<0>(results)[i] <<
        "\tand log-prob = "     << std::get<1>(results)[i] <<
        "\tand prob = "                 << exp(std::get<1>(results)[i]) <<
        "\tand configuration =\t";


        for(int j=0; j<dimNumber; j++){
            for(int k=0; k<isotopeNumbers[j]; k++ )
            {
                std::cout << std::get<2>(results)[m] << " ";
                m++;
            }
            std::cout << '\t';
        }


        std::cout << std::endl;
    }
}

#endif /* BUILDING_R */
