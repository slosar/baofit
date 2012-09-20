// Created 24-May-2012 by David Kirkby (University of California, Irvine) <dkirkby@uci.edu>

#include "baofit/QuasarCorrelationData.h"
#include "baofit/RuntimeError.h"

#include "cosmo/AbsHomogeneousUniverse.h"

#include "likely/AbsBinning.h"

#include <cmath>

namespace local = baofit;

local::QuasarCorrelationData::QuasarCorrelationData(
likely::AbsBinningCPtr axis1, likely::AbsBinningCPtr axis2, likely::AbsBinningCPtr axis3,
double llmin, bool fixCov, cosmo::AbsHomogeneousUniversePtr cosmology)
: AbsCorrelationData(axis1,axis2,axis3,Coordinate)
{
  _initialize(llmin,fixCov,cosmology);
}

local::QuasarCorrelationData::QuasarCorrelationData(
std::vector<likely::AbsBinningCPtr> axes, double llmin, bool fixCov,
cosmo::AbsHomogeneousUniversePtr cosmology)
: AbsCorrelationData(axes,Coordinate)
{
    if(axes.size() != 3) {
        throw RuntimeError("QuasarCorrelationData: expected 3 axes.");
    }
    _initialize(llmin,fixCov,cosmology);
}

void local::QuasarCorrelationData::_initialize(double llmin, bool fixCov,
cosmo::AbsHomogeneousUniversePtr cosmology) {
    _llmin = llmin;
    _fixCov = fixCov; 
    _cosmology = cosmology;
    _lastIndex = -1;
    _arcminToRad = 4*std::atan(1)/(60.*180.);    
}

local::QuasarCorrelationData::~QuasarCorrelationData() { }

local::QuasarCorrelationData *local::QuasarCorrelationData::clone(bool binningOnly) const {
    QuasarCorrelationData *data = binningOnly ?
        new QuasarCorrelationData(getAxisBinning(),_llmin,_fixCov,_cosmology) :
        new QuasarCorrelationData(*this);
    _cloneFinalCuts(*data);
    return data;
}

double local::QuasarCorrelationData::_pkmarg(double kmin, double kmax, double l1, double l2) {
  double f1 (l1==0.0 ? 1.0 : ( sin(kmax*l1)-sin(kmin*l1)  ) / l1);
  double f2 (l2==0.0 ? 1.0 : ( sin(kmax*l2)-sin(kmin*l2)  ) / l2);
  return f1*f2;

}

void local::QuasarCorrelationData::fixCovariance(double k1, double k2, double c) {

    if (!isCovarianceModifiable()) {
        throw RuntimeError("QuasarCorrelationData::fixCovariance: not modifiable.");
    }
    // Make sure that our our data vector is un-weighted.
    getData(*begin());

    // Save values in the outer loop, for re-use in the inner loop.
    std::vector<double> dll;
    dll.reserve(getNBinsWithData());
    std::vector<int> bin(3);
    
    // Lookup the binning along the log-lambda axis.
    likely::AbsBinningCPtr llBins(getAxisBinning()[0]);

    // Loop over all bins.
    for(IndexIterator iter1 = begin(); iter1 != end(); ++iter1) {
        int i1(*iter1);
        // Remember the indices of this 3D bin along our sep,z axes.
        getBinIndices(i1,bin);
        int sepIndex(bin[1]), zIndex(bin[2]);
        // Calculate and save the value of ll - ll0 at the center of this bin.
        double ll(llBins->getBinCenter(bin[0]));
        dll.push_back(ll);
        // Loop over unique pairs (iter1,iter2) with iter2 <= iter1 (which does not
        // necessarily imply that i2 <= i1).
        for(IndexIterator iter2 = begin(); iter2 <= iter1; ++iter2) {
            int i2(*iter2);
            // Check that this bin has the same sep,z indices
            getBinIndices(i2,bin);
            if(bin[1] != sepIndex || bin[2] != zIndex) continue;
            // Calculate (ll1 - ll0)*(ll2 - ll0) using cached values.
            double C(getCovariance(i1,i2));
            C += c * (1.0 + _pkmarg(0.0,k1,dll[i1], dll[i2]) + _pkmarg(k1,k2, dll[i1],dll[i2]));
            setCovariance(i1,i2,C);
        }
    }
}

void local::QuasarCorrelationData::finalize() {

    // First fix Covariance
    if (_fixCov) fixCovariance();

    // Next apply final cuts.
    std::set<int> keep;
    _applyFinalCuts(keep);
    
    // Loop over bins with data.
    for(IndexIterator iter = begin(); iter != end(); ++iter) {
        // Skip bins that have already been cut in _applyFinalCuts
        int index(*iter);
        if(0 == keep.count(index)) continue;        
        // Lookup the value of ll at the center of this bin.
        getBinCenters(index,_binCenter);
        double ll(_binCenter[0]);
        // Keep this bin in our pruned dataset?
        if(ll < _llmin) {
            keep.erase(index);
            continue;
        }
        // Cache the values of (r,mu,z) corresponding to the center of this bin.
        _rLookup.push_back(getRadius(index));
        _muLookup.push_back(getCosAngle(index));
        _zLookup.push_back(getRedshift(index));
    }
    // Prune our dataset down to bins in the keep set.
    prune(keep);
    AbsCorrelationData::finalize();
}

void local::QuasarCorrelationData::transform(double ll, double sep, double dsep, double z,
double &r, double &mu) const {
    double ratio(std::exp(0.5*ll)),zp1(z+1);
    double z1(zp1/ratio-1), z2(zp1*ratio-1);
    double drLos = _cosmology->getLineOfSightComovingDistance(z2) -
        _cosmology->getLineOfSightComovingDistance(z1);
    // Calculate the geometrically weighted mean separation of this bin as
    // Integral[s^2,{s,smin,smax}]/Integral[s,{s,smin,smax}] = s + dsep^2/(12*s)
    double swgt = sep + (dsep*dsep/12)/sep;
    double drPerp = _cosmology->getTransverseComovingScale(z)*(swgt*_arcminToRad);
    double rsq = drLos*drLos + drPerp*drPerp;
    r = std::sqrt(rsq);
    mu = std::abs(drLos)/r;
}

void local::QuasarCorrelationData::_setIndex(int index) const {
    if(index == _lastIndex) return;
    getBinCenters(index,_binCenter);
    getBinWidths(index,_binWidth);
    _zLast = _binCenter[2];
    transform(_binCenter[0],_binCenter[1],_binWidth[1],_zLast,_rLast,_muLast);
    _lastIndex = index;
}

double local::QuasarCorrelationData::getRadius(int index) const {
    if(isFinalized()) return _rLookup[getOffsetForIndex(index)];
    _setIndex(index);
    return _rLast;
}

double local::QuasarCorrelationData::getCosAngle(int index) const {
    if(isFinalized()) return _muLookup[getOffsetForIndex(index)];
    _setIndex(index);
    return _muLast;
}

double local::QuasarCorrelationData::getRedshift(int index) const {
    if(isFinalized()) return _zLookup[getOffsetForIndex(index)];
    _setIndex(index);
    return _zLast;
}
