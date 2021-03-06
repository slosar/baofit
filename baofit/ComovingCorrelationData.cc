// Created 11-Jul-2012 by David Kirkby (University of California, Irvine) <dkirkby@uci.edu>

#include "baofit/ComovingCorrelationData.h"
#include "baofit/RuntimeError.h"

namespace local = baofit;

local::ComovingCorrelationData::ComovingCorrelationData(likely::AbsBinningCPtr rBins,
likely::AbsBinningCPtr muBins, likely::AbsBinningCPtr zBins) 
: AbsCorrelationData(rBins,muBins,zBins,Coordinate), _lastIndex(-1)
{
}

local::ComovingCorrelationData::ComovingCorrelationData(std::vector<likely::AbsBinningCPtr> axes) 
: AbsCorrelationData(axes,Coordinate), _lastIndex(-1)
{
}

local::ComovingCorrelationData::~ComovingCorrelationData() { }

local::ComovingCorrelationData *local::ComovingCorrelationData::clone(bool binningOnly) const {
    ComovingCorrelationData *data = binningOnly ?
        new ComovingCorrelationData(getAxisBinning()) :
        new ComovingCorrelationData(*this);
    _cloneFinalCuts(*data);
    return data;
}

void local::ComovingCorrelationData::finalize() {
    std::set<int> keep;
    _applyFinalCuts(keep);
    prune(keep);
    AbsCorrelationData::finalize();
}

void local::ComovingCorrelationData::_setIndex(int index) const {
    if(index == _lastIndex) return;
    getBinCenters(index,_binCenter);
    _lastIndex = index;
}

double local::ComovingCorrelationData::getRadius(int index) const {
    _setIndex(index);
    return _binCenter[0];
}

double local::ComovingCorrelationData::getCosAngle(int index) const {
    _setIndex(index);
    return _binCenter[1];
}

double local::ComovingCorrelationData::getRedshift(int index) const {
    _setIndex(index);
    return _binCenter[2];
}
