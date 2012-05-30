// Created 31-Jan-2012 by David Kirkby (University of California, Irvine) <dkirkby@uci.edu>

#include "baofit/baofit.h"
#include "cosmo/cosmo.h"
#include "likely/likely.h"
// the following are not part of the public API, so not included by likely.h
#include "likely/MinuitEngine.h"
#include "likely/EngineRegistry.h"

#include "Minuit2/MnUserParameters.h"
#include "Minuit2/FunctionMinimum.h"
#include "Minuit2/MnPrint.h"
#include "Minuit2/MnStrategy.h"
#include "Minuit2/MnMigrad.h"
#include "Minuit2/MnMinos.h"
#include "Minuit2/MnContours.h"

#include "boost/program_options.hpp"
#include "boost/bind.hpp"
#include "boost/ref.hpp"
#include "boost/lexical_cast.hpp"
#include "boost/regex.hpp"
#include "boost/format.hpp"
#include "boost/foreach.hpp"
#include "boost/spirit/include/qi.hpp"
#include "boost/spirit/include/phoenix_core.hpp"
#include "boost/spirit/include/phoenix_operator.hpp"
#include "boost/pointer_cast.hpp"

#include <fstream>
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <cassert>
#include <vector>
#include <set>
#include <algorithm>

// Declare bindings to BLAS,LAPACK routines we need
extern "C" {
    // http://www.netlib.org/lapack/double/dpptrf.f
    void dpptrf_(char const *uplo, int const *n, double *ap, int *info);
    // http://www.netlib.org/lapack/double/dpptri.f
    void dpptri_(char const *uplo, int const *n, double *ap, int *info);
    // http://netlib.org/blas/dspmv.f
    void dspmv_(char const *uplo, int const *n, double const *alpha, double const *ap,
        double const *x, int const *incx, double const *beta, double *y, int const *incy);
    // http://www.netlib.org/blas/dsymm.f
    void dsymm_(char const *side, char const *uplo, int const *m, int const *n,
        double const *alpha, double const *a, int const *lda, double const *b,
        int const *ldb, double const *beta, double *c, int const *ldc);
}

namespace lk = likely;
namespace po = boost::program_options;

std::vector<double> twoStepSampling(
int nBins, double breakpoint,double dlog, double dlin, double eps = 1e-3) {
    assert(breakpoint > 0 && dlog > 0 && dlin > 0 && eps > 0);
    std::vector<double> samplePoints;
    // first sample is at zero.
    samplePoints.push_back(0);
    // next samples are uniformly spaced up to the breakpoint.
    int nUniform = std::floor(breakpoint/dlin);
    for(int k = 1; k <= nUniform; ++k) {
        samplePoints.push_back((k-0.5)*dlin);
    }
    // remaining samples are logarithmically spaced, with log-weighted bin centers.
    double ratio = std::log((breakpoint+dlog)/breakpoint);
    for(int k = 1; k < nBins-nUniform; ++k) {
        samplePoints.push_back(breakpoint*std::exp(ratio*(k-0.5)));
    }
    return samplePoints;
}

void getDouble(std::string::const_iterator const &begin, std::string::const_iterator const &end,
    double &value) {
    // Use boost::spirit::parse instead of the easier boost::lexical_cast since this is
    // a bottleneck when reading many files. For details, see:
    // http://tinodidriksen.com/2011/05/28/cpp-convert-string-to-double-speed/
    std::string tokenString(begin,end);
    char const *tokenPtr = tokenString.c_str();
    boost::spirit::qi::parse(tokenPtr, &tokenPtr[tokenString.size()],
        boost::spirit::qi::double_, value);    
}

void getInt(std::string::const_iterator const &begin, std::string::const_iterator const &end,
    int &value) {
    std::string tokenString(begin,end);
    value = std::atoi(tokenString.c_str());        
}

// Loads a binned correlation function in cosmolib format and returns a BinnedData object.
// The fast option disables regexp checks for valid numeric inputs.
baofit::QuasarCorrelationDataPtr loadCosmolib(std::string dataName,
    likely::AbsBinningCPtr llBins, likely::AbsBinningCPtr sepBins, likely::AbsBinningCPtr zBins,
    cosmo::AbsHomogeneousUniversePtr cosmology, bool verbose, bool icov = false, bool fast = false) {
    // Create the new BinnedData.
    baofit::QuasarCorrelationDataPtr
        binnedData(new baofit::QuasarCorrelationData(llBins,sepBins,zBins,cosmology));
    // General stuff we will need for reading both files.
    std::string line;
    int lineNumber(0);
    // Capturing regexps for positive integer and signed floating-point constants.
    std::string ipat("(0|(?:[1-9][0-9]*))"),fpat("([-+]?[0-9]*\\.?[0-9]+(?:[eE][-+]?[0-9]+)?)");
    if(fast) {
        // Replace validation patterns with simple non-whitespace groups.
        ipat = "(\\S+)";
        fpat = "(\\S+)";
    }
    boost::match_results<std::string::const_iterator> what;
    // Loop over lines in the parameter file.
    std::string paramsName(dataName + ".params");
    std::ifstream paramsIn(paramsName.c_str());
    if(!paramsIn.good()) throw cosmo::RuntimeError("Unable to open " + paramsName);
    boost::regex paramPattern(
        boost::str(boost::format("\\s*%s\\s+%s\\s*\\| Lya covariance 3D \\(%s,%s,%s\\)\\s*")
        % fpat % fpat % fpat % fpat % fpat));
    std::vector<double> axisValues(3);
    while(paramsIn.good() && !paramsIn.eof()) {
        std::getline(paramsIn,line);
        if(paramsIn.eof()) break;
        if(!paramsIn.good()) {
            throw cosmo::RuntimeError("Unable to read line " +
                boost::lexical_cast<std::string>(lineNumber));
        }
        lineNumber++;
        // Parse this line with a regexp.
        if(!boost::regex_match(line,what,paramPattern)) {
            throw cosmo::RuntimeError("Badly formatted params line " +
                boost::lexical_cast<std::string>(lineNumber) + ": '" + line + "'");
        }
        // Expected tokens are [0] value [1] Cinv*d (ignored) [2] logLambda [3] separation [4] redshift.
        int nTokens(5);
        std::vector<double> token(nTokens);
        for(int tok = 0; tok < nTokens; ++tok) {
            getDouble(what[tok+1].first,what[tok+1].second,token[tok]);
        }
        // Add this bin to our dataset.
        //!!addData(token[0],token[2],token[3],token[4]);
        axisValues[0] = token[2];
        axisValues[1] = token[3];
        axisValues[2] = token[4];
        int index = binnedData->getIndex(axisValues);
        binnedData->setData(index,token[0]);        
    }
    //!!finalizeData();
    paramsIn.close();
    if(verbose) {
        std::cout << "Read " << binnedData->getNBinsWithData() << " of "
            << binnedData->getNBinsTotal() << " data values from " << paramsName << std::endl;
    }
    // Loop over lines in the covariance file.
    std::string covName(dataName + (icov ? ".icov" : ".cov"));
    std::ifstream covIn(covName.c_str());
    if(!covIn.good()) throw cosmo::RuntimeError("Unable to open " + covName);
    boost::regex covPattern(boost::str(boost::format("\\s*%s\\s+%s\\s+%s\\s*")
        % ipat % ipat % fpat));
    lineNumber = 0;
    double value;
    int offset1,offset2;
    while(covIn.good() && !covIn.eof()) {
        std::getline(covIn,line);
        if(covIn.eof()) break;
        if(!covIn.good()) {
            throw cosmo::RuntimeError("Unable to read line " +
                boost::lexical_cast<std::string>(lineNumber));
        }
        lineNumber++;
        // Parse this line with a regexp.
        if(!boost::regex_match(line,what,covPattern)) {
            throw cosmo::RuntimeError("Badly formatted cov line " +
                boost::lexical_cast<std::string>(lineNumber) + ": '" + line + "'");
        }
        getInt(what[1].first,what[1].second,offset1);
        getInt(what[2].first,what[2].second,offset2);
        getDouble(what[3].first,what[3].second,value);
        // Add this covariance to our dataset.
        if(icov) value = -value; // !?! see line #388 of Observed2Point.cpp
        //!!addCovariance(offset1,offset2,value,icov);
        int index1 = *(binnedData->begin()+offset1), index2 = *(binnedData->begin()+offset2);
        if(icov) {
            binnedData->setInverseCovariance(index1,index2,value);
        }
        else {
            binnedData->setCovariance(index1,index2,value);
        }
    }
    //!!finalizeCovariance(icov);
    // Check for zero values on the diagonal
    for(likely::BinnedData::IndexIterator iter = binnedData->begin();
    iter != binnedData->end(); ++iter) {
        int index = *iter;
        if(icov) {
            if(0 == binnedData->getInverseCovariance(index,index)) {
                binnedData->setInverseCovariance(index,index,1e-30);
            }
        }
        else {
            if(0 == binnedData->getCovariance(index,index)) {
                binnedData->setCovariance(index,index,1e40);
            }                
        }
    }
    covIn.close();
    if(verbose) {
        int ndata = binnedData->getNBinsWithData();
        int ncov = (ndata*(ndata+1))/2;
        std::cout << "Read " << lineNumber << " of " << ncov
            << " covariance values from " << covName << std::endl;
    }
    return binnedData;
}


class LyaData {
public:
    LyaData(likely::AbsBinningCPtr logLambdaBinning, likely::AbsBinningCPtr separationBinning,
    likely::AbsBinningCPtr redshiftBinning, cosmo::AbsHomogeneousUniversePtr cosmology)
    : _cosmology(cosmology), _logLambdaBinning(logLambdaBinning),
    _separationBinning(separationBinning), _redshiftBinning(redshiftBinning),
    _dataFinalized(false), _covarianceFinalized(false), _compressed(false),
    _binnedData(logLambdaBinning,separationBinning,redshiftBinning)
    {
        assert(logLambdaBinning);
        assert(separationBinning);
        assert(redshiftBinning);
        assert(cosmology);
        _nsep = separationBinning->getNBins();
        _nz = redshiftBinning->getNBins();
        _nBinsTotal = logLambdaBinning->getNBins()*_nsep*_nz;
        _initialized.resize(_nBinsTotal,false);
        _arcminToRad = 4*std::atan(1)/(60.*180.);
    }
    void addData(double value, double logLambda, double separation, double redshift) {
        // Lookup which (ll,sep,z) bin we are in.
        int logLambdaBin(_logLambdaBinning->getBinIndex(logLambda)),
            separationBin(_separationBinning->getBinIndex(separation)),
            redshiftBin(_redshiftBinning->getBinIndex(redshift));
        int index = (logLambdaBin*_nsep + separationBin)*_nz + redshiftBin;
        // Check that input (ll,sep,z) values correspond to bin centers.
        assert(std::fabs(logLambda-_logLambdaBinning->getBinCenter(logLambdaBin)) < 1e-6);
        assert(std::fabs(separation-_separationBinning->getBinCenter(separationBin)) < 1e-6);
        assert(std::fabs(redshift-_redshiftBinning->getBinCenter(redshiftBin)) < 1e-6);
        // Check that we have not already filled this bin.
        assert(!_initialized[index]);
        // Remember this bin.
        _data.push_back(value);
        _initialized[index] = true;
        _index.push_back(index);
        
        std::vector<double> axisValues(3);
        axisValues[0] = logLambda;
        axisValues[1] = separation;
        axisValues[2] = redshift;
        assert(index == _binnedData.getIndex(axisValues));
        _binnedData.setData(index,value);

        // Calculate and save model observables for this bin.
        double r3d,mu,ds(_separationBinning->getBinWidth(separationBin));
        transform(logLambda,separation,redshift,ds,r3d,mu);
        _r3d.push_back(r3d);
        _mu.push_back(mu);
    }
    void finalizeData() {
        int nData = getNData();
        int nCov = (nData*(nData+1))/2;
        //!!_cov.resize(nCov,0);
        _hasCov.resize(nCov,false);
        _dataFinalized = true;
        
        _covariance.reset(new lk::CovarianceMatrix(nData));
    }
    void transform(double ll, double sep, double z, double ds, double &r3d, double &mu) const {
        double ratio(std::exp(0.5*ll)),zp1(z+1);
        double z1(zp1/ratio-1), z2(zp1*ratio-1);
        double drLos = _cosmology->getLineOfSightComovingDistance(z2) -
            _cosmology->getLineOfSightComovingDistance(z1);
        // Calculate the geometrically weighted mean separation of this bin as
        // Integral[s^2,{s,smin,smax}]/Integral[s,{s,smin,smax}] = s + ds^2/(12*s)
        double swgt = sep + (ds*ds/12)/sep;
        double drPerp = _cosmology->getTransverseComovingScale(z)*(swgt*_arcminToRad);
        double rsq = drLos*drLos + drPerp*drPerp;
        r3d = std::sqrt(rsq);
        mu = std::abs(drLos)/r3d;
/**
        std::cout << '(' << ll << ',' << sep << ',' << z << ") => ["
            << z1 << ',' << z2 << ',' << swgt << ';' << drLos << ','
            << drPerp << ',' << mu << ']' << std::endl;
**/
    }
    void addCovariance(int i, int j, double value, bool cov_is_icov) {
        // put into upper-diagonal form col >= row
        int row,col;
        if(i >= j) {
            col = i; row = j;
        }
        else {
            row = i; col = j;
        }
        assert(_dataFinalized);
        assert(row >= 0 && col >= 0 && col < getNData());
        //!!assert(col > row || value > 0); // diagonal elements must be positive for covariance matrix
        int index(row+(col*(col+1))/2); // see http://www.netlib.org/lapack/lug/node123.html
        assert(_hasCov[index] == false);

        //!!_cov[index] = value;

        int ii = _binnedData.getIndexAtOffset(i), jj = _binnedData.getIndexAtOffset(j);
        if(cov_is_icov) {
            _covariance->setInverseCovariance(i,j,value);
            _binnedData.setInverseCovariance(ii,jj,value);
        }
        else {
            _covariance->setCovariance(i,j,value);
            _binnedData.setCovariance(ii,jj,value);
        }

        _hasCov[index] = true;
    }
    void finalizeCovariance(bool cov_is_icov) {
        assert(_dataFinalized);
        int nData = getNData();

        // Check for zero values on the diagonal
        if(cov_is_icov) {
            for(int k = 0; k < nData; ++k) {
                if(0 == _covariance->getInverseCovariance(k,k)) {
                    _covariance->setInverseCovariance(k,k,1e-30);
                }
                int index = _binnedData.getIndexAtOffset(k);
                if(0 == _binnedData.getInverseCovariance(index,index)) {
                    _binnedData.setInverseCovariance(index,index,1e-30);
                }
            }
        }
        else {
            for(int k = 0; k < nData; ++k) {
                if(0 == _covariance->getCovariance(k,k)) {
                    _covariance->setCovariance(k,k,1e40);
                }
                int index = _binnedData.getIndexAtOffset(k);
                if(0 == _binnedData.getCovariance(index,index)) {
                    _binnedData.setCovariance(index,index,1e40);
                }
            }
        }
        /*!*
        if(cov_is_icov) {
            // The values we read into cov actually belong in icov.
            std::swap(_cov,_icov);            
        }
        else {
            // Calculate icov by inverting cov.
            invert(_cov,_icov,getNData());
        }
        // Fill _icovData.
        multiply(_icov,_data,_icovData);
        *!*/

        // All done.
        _covarianceFinalized = true;
        
        _icovData = _data;
        _covariance->multiplyByInverseCovariance(_icovData);
    }
    void reset() {
        _dataFinalized = _covarianceFinalized = false;
        _data.clear();
        _compressed = false;
        
        _covariance.reset();
        _covarianceTilde.reset();
    }
    // A compressed object can only be added to another object.
    void compress() {
        /*!*
        int nData(getNData());
        int nCov = (nData*(nData+1))/2;
        // The following swaps are to force the memory to be free'd.
        std::vector<double>().swap(_zicov);
        std::vector<int>().swap(_zicovIndex);
        for(int k = 0; k < nCov; ++k) {
            float value(_icov[k]);
            if(0 == value) continue;
            _zicov.push_back(value);
            _zicovIndex.push_back(k);
        }
        std::vector<double>().swap(_icov);
        std::vector<double>().swap(_cov);
        std::vector<bool>().swap(_hasCov);
        std::vector<bool>().swap(_initialized);
        *!*/
        _compressed = true;
        
        _covariance->compress();
    }
    void add(LyaData const &other, int repeat = 1) {
        assert(!_dataFinalized && !_covarianceFinalized && !_compressed);
        assert(other._dataFinalized && other._covarianceFinalized);
        int nData(other.getNData());
        //!!int nCov = (nData*(nData+1))/2;
        if(0 == _data.size()) {
            // Allocate empty arrays if this is the first data added.
            std::vector<double>(nData,0).swap(_data);
            std::vector<double>(nData,0).swap(_icovData);
            /*!*
            std::vector<double>(nCov,0).swap(_icov);
            std::vector<double>(nCov,0).swap(_icovTilde);
            *!*/
            // Copy cached data.
            _nBinsTotal = other._nBinsTotal;
            _index = other._index;
            _r3d = other._r3d;
            _mu= other._mu;
            
            _covariance.reset(new lk::CovarianceMatrix(nData));
            _covarianceTilde.reset(new lk::CovarianceMatrix(nData));
            _binnedData = other._binnedData;
            _binnedData.cloneCovariance();
        }
        else {
            assert(nData == getNData());
            _binnedData += other._binnedData;
        }
        for(int k = 0; k < nData; ++k) {
            _icovData[k] += repeat*other._icovData[k];
        }
        double nk(repeat), nk2(repeat*repeat);
        /*!*
        if(other._compressed) {
            int nz(other._zicov.size());
            for(int iz = 0; iz < nz; ++iz) {
                int k = other._zicovIndex[iz];
                _icovTilde[k] += nk*other._zicov[iz];
                _icov[k] += nk2*other._zicov[iz];
            }
        }
        else {
            for(int k = 0; k < nCov; ++k) {
                _icovTilde[k] += nk*other._icov[k];
                _icov[k] += nk2*other._icov[k];
            }
        }
        *!*/

        _covariance->addInverse(*(other._covariance),nk2);
        _covarianceTilde->addInverse(*(other._covariance),nk);
    }
    void prune(double rmin, double rmax, double llmin) {
        std::set<int> keep;
        std::vector<double> binCenters;
        for(int offset = 0; offset < _binnedData.getNBinsWithData(); ++offset) {
            int index = _binnedData.getIndexAtOffset(offset);
            _binnedData.getBinCenters(index,binCenters);
            if(getRadius(offset) >= rmin && getRadius(offset) < rmax && binCenters[0] >= llmin) {
                keep.insert(index);
            }
        }
        std::cout << "Pruning from " << _binnedData.getNBinsWithData() << " to "
            << keep.size() << std::endl;
        _binnedData.prune(keep);
    }
    // Inverts an n by n symmetric matrix in BLAS upper diagonal form
    void invert(std::vector<double> const &original, std::vector<double> &inverse, int n) {
        // Copy original to inverse, element by element.
        inverse = original;
        // Setup LAPACK/BLAS parameters.
        char uplo('U');
        int info(0);
        // Do the Cholesky decomposition of inverse.
        dpptrf_(&uplo,&n,&inverse[0],&info);
        if(0 != info) std::cout << "Cholesky error: info = " << info << std::endl;
        assert(0 == info);
        dpptri_(&uplo,&n,&inverse[0],&info); // Calculate inverse
        if(0 != info) std::cout << "Inverse error: info = " << info << std::endl;
        assert(0 == info);
    }
    // Multiplies a symmetric matrix in BLAS upper diagonal form by invec,
    // storing the result in outvec.
    void multiply(std::vector<double> const &matrix, std::vector<double> const &invec,
    std::vector<double> &outvec) {
        // Get the size from the input vector.
        int n(invec.size());
        // Zero output vector.
        std::vector<double>(n,0).swap(outvec);
        // Setup LAPACK/BLAS parameters.
        char uplo('U');
        int incr(1);
        double alpha(1),beta(0);
        dspmv_(&uplo,&n,&alpha,&matrix[0],&invec[0],&incr,&beta,&outvec[0],&incr);        
    }
    // Returns element [i,j] of a symmetric matrix stored in BLAS upper-diagonal form.
    // see http://www.netlib.org/lapack/lug/node123.html
    double getSymmetric(std::vector<double> const &matrix, int i, int j) const {
        int row,col;
         // put into upper-diagonal form col >= row
        if(i >= j) {
            col = i; row = j;
        }
        else {
            row = i; col = j;
        }
        assert(row >= 0 && col >= 0 && col < getNData());
        int index(row+(col*(col+1))/2);
        return matrix[index];
    }
    // Use fixCovariance to calculate the correct covariance for a bootstrap sample with
    // repetitions. With no repetitions, fixCovariance = false gives the same answer
    // and is faster.
    void finalize(bool fixCovariance) {
        assert(!_dataFinalized && !_covarianceFinalized && !_compressed);
        /*!*
        // Invert _icovTilde into _cov
        invert(_icovTilde,_cov,getNData());
        // Multiply _icovData by this to get final data.
        multiply(_cov,_icovData,_data);
        *!*/
        _data = _icovData;
        _covarianceTilde->multiplyByCovariance(_data);
        
        // Do we want to get the covariance right?
        if(fixCovariance) {
            /*!!
            // Invert the nk^2 weighted inverse-covariance in _icov and save in _cov
            int n(getNData());
            invert(_icov,_cov,n);
            // Multiply _icovTilde * _cov * _icovTilde and store the result in _icov...
            // First, unpack _cov and _icovTilde.
            std::vector<double> covUnpacked(n*n), icovTildeUnpacked(n*n);
            int index(0);
            for(int col = 0; col < n; ++col) {
                for(int row = 0; row <= col; ++row) {
                    int index2 = row*n + col, index3 = col*n + row;
                    covUnpacked[index2] = covUnpacked[index3] = _cov[index];
                    icovTildeUnpacked[index2] = icovTildeUnpacked[index3] = _icovTilde[index];
                    index++;
                }
            }
            // Multiply covUnpacked by icovTildeUnpacked, saving result in tmp
            char side('L'),uplo('U');
            double alpha(1),beta(0);
            std::vector<double> tmp(n*n); // do not need to initialize values when beta=0
            dsymm_(&side,&uplo,&n,&n,&alpha,&covUnpacked[0],&n,&icovTildeUnpacked[0],&n,&beta,
                &tmp[0],&n);
            // Multiply icovTildeUnpacked by tmp, saving result in covUnpacked
            dsymm_(&side,&uplo,&n,&n,&alpha,&icovTildeUnpacked[0],&n,&tmp[0],&n,&beta,
                &covUnpacked[0],&n);
            // Pack covUnpacked back into _icov
            index = 0;
            for(int col = 0; col < n; ++col) {
                for(int row = 0; row <= col; ++row) {
                    _icov[index] = covUnpacked[row*n + col];
                    index++;
                }
            }
            // Calculate _cov from _icov.
            invert(_icov,_cov,getNData());
            !!*/
            
            _covariance->replaceWithTripleProduct(*_covarianceTilde);

        }
        else {
            // We have already inverted _icovTilde into _cov so we only need to
            // copy _icovTilde into _icov.
            /*!!
            _icov.swap(_icovTilde);
            !!*/
            _covariance = _covarianceTilde;
        }
        // Delete temporary storage
        /*!!
        std::vector<double>().swap(_icovTilde);
        !!*/
        _covarianceTilde.reset();
        // All done.
        _dataFinalized = _covarianceFinalized = true;
    }
    int getSize() const { return _nBinsTotal; }
    int getNData() const { return _data.size(); }
    int getNCov() const { return (int)std::count(_hasCov.begin(),_hasCov.end(),true); }
    int getIndex(int k) const { return _index[k]; }
    double getData(int k) const { return _data[k]; }
    double getVariance(int k) const { return _covariance->getCovariance(k,k); }
    //!!void setVariance(int k, double value) { _cov[(k*(k+3))/2] = value; }
    double getRadius(int k) const { return _r3d[k]; }
    double getCosAngle(int k) const { return _mu[k]; }
    double getRedshift(int k) const { return _redshiftBinning->getBinCenter(_index[k] % _nz); }
    likely::AbsBinningCPtr getLogLambdaBinning() const { return _logLambdaBinning; }
    likely::AbsBinningCPtr getSeparationBinning() const { return _separationBinning; }
    likely::AbsBinningCPtr getRedshiftBinning() const { return _redshiftBinning; }
    double calculateChiSquare(std::vector<double> &delta) {
        assert(delta.size() == getNData());
        /*!!
        // Calculate C^(-1).delta
        multiply(_icov,delta,_icovDelta);
        // Calculate chi2 = delta(t).C^(-1).delta
        double chi2(0);
        for(int k = 0; k < getNData(); ++k) {
            chi2 += delta[k]*_icovDelta[k];
        }
        !!*/
        return _covariance->chiSquare(delta);
    }
    void applyTheoryOffsets(baofit::AbsCorrelationModelCPtr model,
    std::vector<double> const &pfit, std::vector<double> const &pnew) {
        assert(model);
        int nData(getNData());
        for(int k = 0; k < nData; ++k) {
            double r = getRadius(k), mu = getCosAngle(k), z = getRedshift(k);
            double offset = model->evaluate(r,mu,z,pnew) - model->evaluate(r,mu,z,pfit);
            _data[k] += offset;
        }
        /*!!
        // Uncompress _icov if necessary
        if(_compressed) {
            int nCov = (nData*(nData+1))/2;
            std::vector<double>(nCov,0).swap(_icov);
            for(int iz = 0; iz < _zicov.size(); ++iz) {
                int k = _zicovIndex[iz];
                _icov[k] = _zicov[iz];
            }
        }        
        // Update _icovData = C^(-1).data
        multiply(_icov,_data,_icovData);        
        // Remove the uncompressed _icov if necessary.
        if(_compressed) {
            std::vector<double>().swap(_icov);
        }
        !!*/
        bool compressed(_covariance->isCompressed());
        _icovData = _data;
        _covariance->multiplyByInverseCovariance(_icovData);
        if(compressed) _covariance->compress();
    }
    void getDouble(std::string::const_iterator const &begin, std::string::const_iterator const &end,
        double &value) const {
        // Use boost::spirit::parse instead of the easier boost::lexical_cast since this is
        // a bottleneck when reading many files. For details, see:
        // http://tinodidriksen.com/2011/05/28/cpp-convert-string-to-double-speed/
        std::string tokenString(begin,end);
        char const *tokenPtr = tokenString.c_str();
        boost::spirit::qi::parse(tokenPtr, &tokenPtr[tokenString.size()],
            boost::spirit::qi::double_, value);    
    }
    void getInt(std::string::const_iterator const &begin, std::string::const_iterator const &end,
        int &value) const {
        std::string tokenString(begin,end);
        value = std::atoi(tokenString.c_str());        
    }
    // The fast option disables regexp checks for valid numeric inputs.
    void load(std::string dataName, bool verbose, bool icov = false, bool fast = false) {
        // General stuff we will need for reading both files.
        std::string line;
        int lineNumber(0);
        // Capturing regexps for positive integer and signed floating-point constants.
        std::string ipat("(0|(?:[1-9][0-9]*))"),fpat("([-+]?[0-9]*\\.?[0-9]+(?:[eE][-+]?[0-9]+)?)");
        if(fast) {
            // Replace validation patterns with simple non-whitespace groups.
            ipat = "(\\S+)";
            fpat = "(\\S+)";
        }
        boost::match_results<std::string::const_iterator> what;
        // Loop over lines in the parameter file.
        std::string paramsName(dataName + ".params");
        std::ifstream paramsIn(paramsName.c_str());
        if(!paramsIn.good()) throw cosmo::RuntimeError("Unable to open " + paramsName);
        boost::regex paramPattern(
            boost::str(boost::format("\\s*%s\\s+%s\\s*\\| Lya covariance 3D \\(%s,%s,%s\\)\\s*")
            % fpat % fpat % fpat % fpat % fpat));
        while(paramsIn.good() && !paramsIn.eof()) {
            std::getline(paramsIn,line);
            if(paramsIn.eof()) break;
            if(!paramsIn.good()) {
                throw cosmo::RuntimeError("Unable to read line " +
                    boost::lexical_cast<std::string>(lineNumber));
            }
            lineNumber++;
            // Parse this line with a regexp.
            if(!boost::regex_match(line,what,paramPattern)) {
                throw cosmo::RuntimeError("Badly formatted params line " +
                    boost::lexical_cast<std::string>(lineNumber) + ": '" + line + "'");
            }
            int nTokens(5);
            std::vector<double> token(nTokens);
            for(int tok = 0; tok < nTokens; ++tok) {
                getDouble(what[tok+1].first,what[tok+1].second,token[tok]);
            }
            // Add this bin to our dataset. Second value token[1] might be non-zero, in which case
            //  it is Cinv*d from the quadratic estimator, but we just ignore it.
            addData(token[0],token[2],token[3],token[4]);
        }
        finalizeData();
        paramsIn.close();
        if(verbose) {
            std::cout << "Read " << getNData() << " of " << getSize()
                << " data values from " << paramsName << std::endl;
        }
        // Loop over lines in the covariance file.
        std::string covName(dataName + (icov ? ".icov" : ".cov"));
        std::ifstream covIn(covName.c_str());
        if(!covIn.good()) throw cosmo::RuntimeError("Unable to open " + covName);
        boost::regex covPattern(boost::str(boost::format("\\s*%s\\s+%s\\s+%s\\s*")
            % ipat % ipat % fpat));
        lineNumber = 0;
        double value;
        int index1,index2;
        while(covIn.good() && !covIn.eof()) {
            std::getline(covIn,line);
            if(covIn.eof()) break;
            if(!covIn.good()) {
                throw cosmo::RuntimeError("Unable to read line " +
                    boost::lexical_cast<std::string>(lineNumber));
            }
            lineNumber++;
            // Parse this line with a regexp.
            if(!boost::regex_match(line,what,covPattern)) {
                throw cosmo::RuntimeError("Badly formatted cov line " +
                    boost::lexical_cast<std::string>(lineNumber) + ": '" + line + "'");
            }
            getInt(what[1].first,what[1].second,index1);
            getInt(what[2].first,what[2].second,index2);
            getDouble(what[3].first,what[3].second,value);
            // Add this covariance to our dataset.
            if(icov) value = -value; // !?! see line #388 of Observed2Point.cpp
            addCovariance(index1,index2,value,icov);
        }
        finalizeCovariance(icov);
        covIn.close();
        if(verbose) {
            int ndata = getNData();
            int ncov = (ndata*(ndata+1))/2;
            std::cout << "Read " << getNCov() << " of " << ncov
                << " covariance values from " << covName << std::endl;
        }
    }
    //lk::CovarianceMatrixCPtr getCovariance() { return _covariance; }
//private:
public:
    likely::AbsBinningCPtr _logLambdaBinning, _separationBinning, _redshiftBinning;
    cosmo::AbsHomogeneousUniversePtr _cosmology;
    std::vector<double> _data, /*_cov, _icov, _icovTilde,*/ _r3d, _mu, _icovDelta, _icovData;
    std::vector<double> _zicov;
    std::vector<bool> _initialized, _hasCov;
    std::vector<int> _index, _zicovIndex;
    int _ndata,_nsep,_nz,_nBinsTotal;
    double _arcminToRad;
    bool _dataFinalized, _covarianceFinalized, _compressed;
    
    boost::shared_ptr<lk::CovarianceMatrix> _covariance, _covarianceTilde;
    lk::BinnedData _binnedData;
    
}; // LyaData

typedef boost::shared_ptr<LyaData> LyaDataPtr;

typedef std::pair<double,double> ContourPoint;
typedef std::vector<ContourPoint> ContourPoints;

class Parameter {
public:
    Parameter(std::string const &name, double value, double error, bool floating = false)
    : _name(name), _value(value), _initialValue(value),
    _error(error), _initialError(error), _floating(floating)
    { }
    void fix(double value) {
        _value = value;
        _floating = false;
    }
    void setValue(double value) { _value = value; }
    bool isFloating() const { return _floating; }
    double getValue() const { return _value; }
    void setError(double error) { _error = error; }
    double getError() const { return _error; }
    std::string getName() const { return _name; }
    void reset() { _value = _initialValue; _error = _initialError; }
private:
    std::string _name;
    double _value, _initialValue, _error, _initialError;
    bool _floating;
}; // Parameter

class LyaBaoLikelihood {
public:
    LyaBaoLikelihood(baofit::AbsCorrelationDataCPtr data, baofit::AbsCorrelationModelCPtr model,
    double rmin, double rmax,
    bool fixLinear, bool fixBao, bool fixScale, bool noBBand, double initialAmp, double initialScale)
    : _data(data), _model(model), _rmin(rmin), _rmax(rmax), _errorScale(1), _ncalls(0) {
        assert(data);
        assert(model);
        assert(rmax > rmin);
        _params.push_back(Parameter("Alpha",3.8,0.3,false)); //!fixLinear));
        _params.push_back(Parameter("BB",0.34,0.03,!fixLinear && (fixBao || noBBand)));
        _params.push_back(Parameter("Beta",1.0,0.1,!fixLinear && (fixBao || noBBand)));
        _params.push_back(Parameter("BAO Ampl",initialAmp,0.15,!fixBao));
        _params.push_back(Parameter("BAO Scale",initialScale,0.02,!fixBao && !fixScale));
        _params.push_back(Parameter("BB xio",0,0.001,!noBBand));
        _params.push_back(Parameter("BB a0",0,0.2,!noBBand));
        _params.push_back(Parameter("BB a1",0,2,!noBBand));
        _params.push_back(Parameter("BB a2",0,2,!noBBand));
    }
    void setErrorScale(double scale) {
        assert(scale > 0);
        _errorScale = scale;
    }
    double operator()(lk::Parameters const &params) {
        // Loop over the dataset bins.
        std::vector<double> pred;
        pred.reserve(_data->getNBinsWithData());
        int offset(0);
        for(baofit::AbsCorrelationData::IndexIterator iter = _data->begin(); iter != _data->end(); ++iter) {
            int index(*iter);
            double r = _data->getRadius(index);
            assert(r >= _rmin && r < _rmax);
            double mu = _data->getCosAngle(index);
            double z = _data->getRedshift(index);
            double predicted = _model->evaluate(r,mu,z,params);
            pred.push_back(predicted);
            //!!DK
            if(0 == _ncalls && offset++ < 5) {
                std::cout << "rr,mu,z = " << r << ',' << mu << ',' << z
                    << " obs=" << _data->getData(index) << ", pred=" << predicted << std::endl;
            }
            //!!DK
        }
        _ncalls++;
        // UP=0.5 is already hardcoded so we need a factor of 2 here since we are
        // calculating a chi-square. Apply an additional factor of _errorScale to
        // allow different error contours to be calculated.
        return 0.5*_data->chiSquare(pred)/_errorScale;
    }
    int getNPar() const { return _params.size(); }
    void initialize(lk::MinuitEngine::StatePtr initialState) {
        BOOST_FOREACH(Parameter &param, _params) {
            param.reset();
            double value(param.getValue());
            if(param.isFloating()) {
                double error = param.getError();
                initialState->Add(param.getName(),value,error);
            }
            else {
                initialState->Add(param.getName(),value,0);
                initialState->Fix(param.getName());
            }
        }
    }
    /**
    void dump(std::string const &filename, lk::Parameters const &params,
    std::vector<ContourPoints> const &contourData, int modelBins) {
        std::ofstream out(filename.c_str());
        // Dump binning info first
        likely::AbsBinningCPtr llbins(_data->getLogLambdaBinning()), sepbins(_data->getSeparationBinning()),
            zbins(_data->getRedshiftBinning());
        llbins->dump(out);
        sepbins->dump(out);
        zbins->dump(out);
        // Dump the number of data bins, the number of model bins, and the number of contour points.
        int ncontour = (0 == contourData.size()) ? 0 : contourData[0].size();
        out << _data->getNData() << ' ' << modelBins << ' ' << ncontour << std::endl;
        // Dump the number of parameters and their best-fit values.
        out << params.size();
        BOOST_FOREACH(double const &pValue, params) out << ' ' << pValue;
        out << std::endl;
        // Dump binned data and most recent pulls.
        for(int k= 0; k < _data->getNData(); ++k) {
            double r = _data->getRadius(k);
            double mu = _data->getCosAngle(k);
            double z = _data->getRedshift(k);
            double obs = _data->getData(k);
            double pull = 0;
            if(r >= _rmin && r <= _rmax) {
                // TODO: change 1/sqrt(cov) to sqrt(icov) ?
                double var = _data->getVariance(k);
                double pred = _model->evaluate(r,mu,z,params);
                pull = (obs-pred)/std::sqrt(var);
            }
            int index = _data->getIndex(k);
            out << index << ' ' << obs << ' ' << pull << std::endl;
        }
        // Dump high-resolution uniformly-binned model calculation.
        // Calculate and dump the model binning limits.
        double sepMin = sepbins->getBinLowEdge(0), sepMax = sepbins->getBinLowEdge(sepbins->getNBins());
        likely::UniformBinning sepModel(sepMin,sepMax,modelBins);
        double llMin = llbins->getBinLowEdge(0), llMax = llbins->getBinLowEdge(llbins->getNBins());
        likely::UniformBinning llModel(llMin,llMax,modelBins);
        double r,mu;
        for(int iz = 0; iz < zbins->getNBins(); ++iz) {
            double z = zbins->getBinCenter(iz);
            for(int isep = 0; isep < modelBins; ++isep) {
                double sep = sepModel.getBinCenter(isep);
                double ds = sepModel.getBinWidth(isep);
                for(int ill = 0; ill < modelBins; ++ill) {
                    double ll = llModel.getBinCenter(ill);
                    _data->transform(ll,sep,z,ds,r,mu);
                    double pred = _model->evaluate(r,mu,z,params);
                    out << r << ' ' << pred << std::endl;
                }
            }
        }
        // Dump 2-parameter contours if we have any.
        if(ncontour) {
            BOOST_FOREACH(ContourPoints const &points, contourData) {
                BOOST_FOREACH(ContourPoint const &point, points) {
                    out << point.first << ' ' << point.second << std::endl;
                }
            }
        }
        out.close();
    }
    **/
private:
    baofit::AbsCorrelationDataCPtr _data;
    baofit::AbsCorrelationModelCPtr _model;
    std::vector<Parameter> _params;
    double _rmin, _rmax, _errorScale;
    int _ncalls;
}; // LyaBaoLikelihood

int main(int argc, char **argv) {
    
    // Configure command-line option processing
    po::options_description cli("BAO fitting");
    double OmegaMatter,hubbleConstant,zref,minll,dll,dll2,minsep,dsep,minz,dz,rmin,rmax,llmin;
    int nll,nsep,nz,ncontour,modelBins,maxPlates,bootstrapTrials,bootstrapSize,randomSeed;
    std::string modelrootName,fiducialName,nowigglesName,broadbandName,dataName,dumpName;
    double initialAmp,initialScale;
    std::string platelistName,platerootName,bootstrapSaveName,bootstrapCurvesName;
    cli.add_options()
        ("help,h", "Prints this info and exits.")
        ("verbose", "Prints additional information.")
        ("omega-matter", po::value<double>(&OmegaMatter)->default_value(0.27),
            "Present-day value of OmegaMatter.")
        ("hubble-constant", po::value<double>(&hubbleConstant)->default_value(0.7),
            "Present-day value of the Hubble parameter h = H0/(100 km/s/Mpc).")
        ("modelroot", po::value<std::string>(&modelrootName)->default_value(""),
                "Common path to prepend to all model filenames.")
        ("fiducial", po::value<std::string>(&fiducialName)->default_value(""),
            "Fiducial correlation functions will be read from <name>.<ell>.dat with ell=0,2,4.")
        ("nowiggles", po::value<std::string>(&nowigglesName)->default_value(""),
            "No-wiggles correlation functions will be read from <name>.<ell>.dat with ell=0,2,4.")
        ("broadband", po::value<std::string>(&broadbandName)->default_value(""),
            "Broadband models will be read from <name>bb<x>.<ell>.dat with x=c,1,2 and ell=0,2,4.")
        ("zref", po::value<double>(&zref)->default_value(2.25),
            "Reference redshift.")
        ("rmin", po::value<double>(&rmin)->default_value(0),
            "Minimum 3D comoving separation (Mpc/h) to use in fit.")
        ("rmax", po::value<double>(&rmax)->default_value(200),
            "Maximum 3D comoving separation (Mpc/h) to use in fit.")
        ("llmin", po::value<double>(&llmin)->default_value(0),
            "Minimum value of log(lam2/lam1) to use in fit.")
        ("data", po::value<std::string>(&dataName)->default_value(""),
            "3D covariance data will be read from <data>.params and <data>.cov")
        ("platelist", po::value<std::string>(&platelistName)->default_value(""),
            "3D covariance data will be read from individual plate datafiles listed in this file.")
        ("plateroot", po::value<std::string>(&platerootName)->default_value(""),
            "Common path to prepend to all plate datafiles listed in the platelist.")
        ("max-plates", po::value<int>(&maxPlates)->default_value(0),
            "Maximum number of plates to load (zero uses all available plates).")
        ("fast-load", "Bypasses numeric input validation when reading data.")
        ("bootstrap-trials", po::value<int>(&bootstrapTrials)->default_value(0),
            "Number of bootstrap trials to run if a platelist was provided.")
        ("bootstrap-size", po::value<int>(&bootstrapSize)->default_value(0),
            "Size of each bootstrap trial or zero to use the number of plates.")
        ("bootstrap-save", po::value<std::string>(&bootstrapSaveName)->default_value("bstrials.txt"),
            "Name of file to write with results of each bootstrap trial.")
        ("bootstrap-curves", po::value<std::string>(&bootstrapCurvesName)->default_value(""),
            "Name of file to write individual bootstrap fit multipole curves to.")
        ("naive-covariance", "Uses the naive covariance matrix for each bootstrap trial.")
        ("null-hypothesis", "Applies theory offsets to simulate the null hypothesis.")
        ("random-seed", po::value<int>(&randomSeed)->default_value(1966),
            "Random seed to use for generating bootstrap samples.")
        ("minll", po::value<double>(&minll)->default_value(0.0002),
            "Minimum log(lam2/lam1).")
        ("dll", po::value<double>(&dll)->default_value(0.004),
            "log(lam2/lam1) binsize.")
        ("dll2", po::value<double>(&dll2)->default_value(0),
            "log(lam2/lam1) second binsize parameter for two-step binning.")
        ("nll", po::value<int>(&nll)->default_value(14),
            "Maximum number of log(lam2/lam1) bins.")
        ("minsep", po::value<double>(&minsep)->default_value(0),
            "Minimum separation in arcmins.")
        ("dsep", po::value<double>(&dsep)->default_value(10),
            "Separation binsize in arcmins.")
        ("nsep", po::value<int>(&nsep)->default_value(14),
            "Maximum number of separation bins.")
        ("minz", po::value<double>(&minz)->default_value(1.7),
            "Minimum redshift.")
        ("dz", po::value<double>(&dz)->default_value(1.0),
            "Redshift binsize.")
        ("nz", po::value<int>(&nz)->default_value(2),
            "Maximum number of redshift bins.")
        ("dump", po::value<std::string>(&dumpName)->default_value(""),
            "Filename for dumping fit results.")
        ("ncontour",po::value<int>(&ncontour)->default_value(0),
            "Number of contour points to calculate in BAO parameters.")
        ("model-bins", po::value<int>(&modelBins)->default_value(200),
            "Number of high-resolution uniform bins to use for dumping best fit model.")
        ("minos", "Runs MINOS to improve error estimates.")
        ("fix-linear", "Fix linear bias parameters alpha, bias, beta.")
        ("fix-bao", "Fix BAO scale and amplitude parameters.")
        ("fix-scale", "Fix BAO scale parameter (amplitude floating).")
        ("no-bband", "Do not add any broadband contribution to the correlation function.")
        ("initial-amp", po::value<double>(&initialAmp)->default_value(1),
            "Initial value for the BAO amplitude parameter.")
        ("initial-scale", po::value<double>(&initialScale)->default_value(1),
            "Initial value for the BAO scale parameter.")
        ;

    // Do the command line parsing now.
    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, cli), vm);
        po::notify(vm);
    }
    catch(std::exception const &e) {
        std::cerr << "Unable to parse command line options: " << e.what() << std::endl;
        return -1;
    }
    if(vm.count("help")) {
        std::cout << cli << std::endl;
        return 1;
    }
    bool verbose(vm.count("verbose")), minos(vm.count("minos")), fastLoad(vm.count("fast-load")),
        fixLinear(vm.count("fix-linear")), fixBao(vm.count("fix-bao")), fixScale(vm.count("fix-scale")),
        noBBand(vm.count("no-bband")), naiveCovariance(vm.count("naive-covariance")),
        nullHypothesis(vm.count("null-hypothesis"));

    // Check for the required filename parameters.
    if(0 == dataName.length() && 0 == platelistName.length()) {
        std::cerr << "Missing required parameter --data or --platelist." << std::endl;
        return -1;
    }
    if(0 == fiducialName.length()) {
        std::cerr << "Missing required parameter --fiducial." << std::endl;
        return -1;
    }
    if(0 == nowigglesName.length()) {
        std::cerr << "Missing required parameter --nowiggles." << std::endl;
        return -1;
    }
    if(0 == broadbandName.length()) {
        std::cerr << "Missing required parameter --broadband." << std::endl;
        return -1;
    }

    // Initialize the cosmology calculations we will need.
    cosmo::AbsHomogeneousUniversePtr cosmology;
    baofit::AbsCorrelationModelCPtr model;
    try {
        // Build the homogeneous cosmology we will use.
        cosmology.reset(new cosmo::LambdaCdmRadiationUniverse(OmegaMatter,0,hubbleConstant));
        
         // Build our fit model from tabulated ell=0,2,4 correlation functions on disk.
         model.reset(new baofit::BaoCorrelationModel(
             modelrootName,fiducialName,nowigglesName,broadbandName,zref,
             initialAmp,initialScale,fixLinear,fixBao,fixScale,noBBand));

        if(verbose) std::cout << "Cosmology initialized." << std::endl;
    }
    catch(cosmo::RuntimeError const &e) {
        std::cerr << "ERROR during cosmology initialization:\n  " << e.what() << std::endl;
        return -2;
    }
    catch(lk::RuntimeError const &e) {
        std::cerr << "ERROR during cosmology initialization:\n  " << e.what() << std::endl;
        return -2;
    }
    
    // Load the data we will fit.
    LyaDataPtr data;
    std::vector<LyaDataPtr> plateData;
    baofit::QuasarCorrelationDataPtr binnedData;
    std::vector<baofit::QuasarCorrelationDataCPtr> plateBinnedData;
    try {
        // Initialize the (logLambda,separation,redshift) binning from command-line params.
        likely::AbsBinningCPtr llBins,
            sepBins(new likely::UniformBinning(minsep,minsep+nsep*dsep,nsep)),
            zBins(new likely::UniformSampling(minz+0.5*dz,minz+(nz-0.5)*dz,nz));
        if(0 == dll2) {
            llBins.reset(new likely::UniformBinning(minll,minll+nll*dll,nll));
        }
        else {
            llBins.reset(new likely::NonUniformSampling(twoStepSampling(nll,minll,dll,dll2)));
        }
        // Initialize the dataset we will fill.
        data.reset(new LyaData(llBins,sepBins,zBins,cosmology));
        if(0 < dataName.length()) {
            // Load a single dataset.
            data->load(dataName,verbose,false,fastLoad);
            binnedData = loadCosmolib(dataName,llBins,sepBins,zBins,cosmology,verbose,false,fastLoad);
        }
        else {
            // Load individual plate datasets.
            std::string plateName;
            boost::format platefile("%s%s");
            platelistName = platerootName + platelistName;
            std::ifstream platelist(platelistName.c_str());
            if(!platelist.good()) {
                std::cerr << "Unable to open platelist file " << platelistName << std::endl;
                return -1;
            }
            while(platelist.good() && !platelist.eof()) {
                platelist >> plateName;
                if(platelist.eof()) break;
                if(!platelist.good()) {
                    std::cerr << "Error while reading platelist from " << platelistName << std::endl;
                    return -1;
                }
                std::string filename(boost::str(platefile % platerootName % plateName));

                LyaDataPtr plate(new LyaData(llBins,sepBins,zBins,cosmology));
                plate->load(filename,verbose,true,fastLoad);
                plate->compress();
                plateData.push_back(plate);
                data->add(*plate);

                baofit::QuasarCorrelationDataCPtr plateBinned =
                    loadCosmolib(filename,llBins,sepBins,zBins,cosmology,verbose,true,fastLoad);
                plateBinned->compress();
                if(plateBinnedData.empty()) {
                    binnedData.reset(new baofit::QuasarCorrelationData(*plateBinned));
                    binnedData->cloneCovariance();
                }
                else {
                    *binnedData += *plateBinned;
                }
                plateBinnedData.push_back(plateBinned);

                if(plateData.size() == maxPlates) break;
            }
            platelist.close();
            data->finalize(false);
        }
        //!!DK
        std::vector<double> coords;
        for(int offset = 0; offset < 10; ++offset) {
            int index = binnedData->getIndexAtOffset(offset);
            binnedData->getBinCenters(index,coords);
            std::cout << "Covariance3D[" << offset << "] idx=" << index << ", ll="
                << coords[0] << ", sep=" << coords[1] << ", z=" << coords[2]
                << ", r=" << binnedData->getRadius(index) << ", mu=" << binnedData->getCosAngle(index)
                << ", z=" << binnedData->getRedshift(index)
                << ", value=" << binnedData->getData(index) << std::endl;
        }
        //!!DK
        
        data->prune(rmin,rmax,llmin);
        
        binnedData->finalize(rmin,rmax,llmin);

        for(int offset = 0; offset < 10; ++offset) {
            int index = binnedData->getIndexAtOffset(offset);
            binnedData->getBinCenters(index,coords);
            std::cout << "Covariance3D[" << offset << "] idx=" << index << ", ll="
                << coords[0] << ", sep=" << coords[1] << ", z=" << coords[2]
                << ", r=" << binnedData->getRadius(index) << ", mu=" << binnedData->getCosAngle(index)
                << ", z=" << binnedData->getRedshift(index)
                << ", value=" << binnedData->getData(index) << std::endl;
        }
    }
    catch(cosmo::RuntimeError const &e) {
        std::cerr << "ERROR while reading data:\n  " << e.what() << std::endl;
        return -2;
    }
    
    // Minimize the -log(Likelihood) function.
    try {
        baofit::CorrelationFit fitEngine(binnedData,model);
        lk::FunctionMinimumPtr fitResult = fitEngine.fit("mn2::vmetric");
        fitResult->printToStream(std::cout);
        
        lk::GradientCalculatorPtr gcptr;
        LyaBaoLikelihood nll(binnedData,model,rmin,rmax,fixLinear,fixBao,fixScale,noBBand,
            initialAmp,initialScale);
        lk::FunctionPtr fptr(new lk::Function(boost::ref(nll)));

        int npar(nll.getNPar());
        lk::AbsEnginePtr engine = lk::getEngine("mn2::vmetric",fptr,gcptr,model->getParameters());
        lk::MinuitEngine &minuit = dynamic_cast<lk::MinuitEngine&>(*engine);        
        lk::MinuitEngine::StatePtr initialState(new ROOT::Minuit2::MnUserParameterState());
        nll.initialize(initialState);
        std::cout << *initialState;
        
        ROOT::Minuit2::MnStrategy strategy(1); // lo(0),med(1),hi(2)
        ROOT::Minuit2::MnMigrad fitter((ROOT::Minuit2::FCNBase const&)minuit,*initialState,strategy); 

        int maxfcn = 100*npar*npar;
        double edmtol = 0.1;
        ROOT::Minuit2::FunctionMinimum fmin = fitter(maxfcn,edmtol);
        
        if(minos) {
            ROOT::Minuit2::MnMinos minosError((ROOT::Minuit2::FCNBase const&)minuit,fmin,strategy);
            for(int ipar = 0; ipar < npar; ++ipar) {
                std::pair<double,double> error = minosError(ipar,maxfcn);
                std::cout << "MINOS error[" << ipar << "] = +" << error.second
                    << ' ' << error.first << std::endl;
            }
        }
        
        std::cout << fmin;
        std::cout << fmin.UserCovariance();
        std::cout << fmin.UserState().GlobalCC();
        
        // Remember the best-fit parameters and errors.
        std::vector<double> bestParams = fmin.UserParameters().Params(),
            bestErrors = fmin.UserParameters().Errors();
        double bestFval = fmin.Fval();
        
        std::vector<ContourPoints> contourData;
        if(ncontour > 0) {
            if(verbose) {
                std::cout << "Calculating contours with " << ncontour << " points..." << std::endl;
            }
            // 95% CL (see http://wwwasdoc.web.cern.ch/wwwasdoc/minuit/node33.html)
            // Calculate in mathematica using:
            // Solve[CDF[ChiSquareDistribution[2], x] == 0.95, x]
            nll.setErrorScale(5.99146);
            fmin = fitter(maxfcn,edmtol);
            ROOT::Minuit2::MnContours contours95((ROOT::Minuit2::FCNBase const&)minuit,fmin,strategy);
            // Parameter indices: 1=bias, 2=beta, 3=BAO amp, 4=BAO scale, 5=bband a1/10, 6=bband a2/1000
            contourData.push_back(contours95(5,6,ncontour));
            contourData.push_back(contours95(4,6,ncontour));
            contourData.push_back(contours95(1,6,ncontour));
            contourData.push_back(contours95(5,3,ncontour));
            contourData.push_back(contours95(4,3,ncontour));
            contourData.push_back(contours95(1,3,ncontour));            
            contourData.push_back(contours95(5,2,ncontour));
            contourData.push_back(contours95(4,2,ncontour));
            contourData.push_back(contours95(1,2,ncontour));
            // 68% CL
            nll.setErrorScale(2.29575);
            fmin = fitter(maxfcn,edmtol);
            ROOT::Minuit2::MnContours contours68((ROOT::Minuit2::FCNBase const&)minuit,fmin,strategy);
            contourData.push_back(contours68(5,6,ncontour));
            contourData.push_back(contours68(4,6,ncontour));
            contourData.push_back(contours68(1,6,ncontour));
            contourData.push_back(contours68(5,3,ncontour));
            contourData.push_back(contours68(4,3,ncontour));
            contourData.push_back(contours68(1,3,ncontour));            
            contourData.push_back(contours68(5,2,ncontour));
            contourData.push_back(contours68(4,2,ncontour));
            contourData.push_back(contours68(1,2,ncontour));
            // reset
            nll.setErrorScale(1);
        }
        
        // Simulate the null hypothesis by applying theory offsets to each plate, if requested.
        if(nullHypothesis) {
            std::vector<double> nullParams(bestParams);
            nullParams[3] = 0; // BAO peak amplitude
            BOOST_FOREACH(LyaDataPtr plate, plateData) {
                plate->applyTheoryOffsets(model,bestParams,nullParams);
            }
        }
        
        int nplates(plateData.size()), nInvalid(0);
        if(0 < bootstrapTrials && 0 < nplates) {
            lk::Random &random(lk::Random::instance());
            random.setSeed(randomSeed);
            boost::scoped_array<lk::WeightedAccumulator>
                accumulators(new lk::WeightedAccumulator[npar+1]);
            if(0 == bootstrapSize) bootstrapSize = nplates;
            std::ofstream out(bootstrapSaveName.c_str());
            out << "trial nuniq alpha bias beta amp scale xio a0 a1 a2 chisq" << std::endl;
            boost::scoped_ptr<std::ofstream> curvesOut;
            if(0 < bootstrapCurvesName.length()) {
                curvesOut.reset(new std::ofstream(bootstrapCurvesName.c_str()));
            }
            for(int k = 0; k < bootstrapTrials; ++k) {
                // First, decide how many copies of each plate to use in this trial.
                std::vector<double> counter(nplates,0);
                for(int p = 0; p < bootstrapSize; ++p) {
                    // Pick a random plate to use in this trial.
                    int index = (int)std::floor(random.getUniform()*nplates);
                    // Keep track of how many times we use this plate.
                    counter[index]++;
                }
                // Next, build the dataset for this trial.
                data->reset();
                for(int index = 0; index < nplates; ++index) {
                    int repeat = counter[index];
                    if(0 < repeat) data->add(*plateData[index],repeat);
                }
                data->finalize(!naiveCovariance);
                // Count total number of different plates used.
                int nuniq = nplates - std::count(counter.begin(),counter.end(),0);
                // Reset parameters to their initial values.
                initialState.reset(new ROOT::Minuit2::MnUserParameterState());
                nll.initialize(initialState);
                // Do the fit.
                ROOT::Minuit2::MnMigrad
                    bsFitter((ROOT::Minuit2::FCNBase const&)minuit,*initialState,strategy);
                fmin = bsFitter(maxfcn,edmtol);
                if(fmin.IsValid()) {
                    // Save the fit results and accumulate bootstrap stats for each parameter.
                    out << k << ' ' << nuniq << ' ';
                    std::vector<double> params = fmin.UserParameters().Params();
                    for(int i = 0; i < npar; ++i) {
                        double value = params[i];
                        accumulators[i].accumulate(value);
                        out << value << ' ';
                    }
                    out << fmin.Fval() << std::endl;
                    accumulators[npar].accumulate(fmin.Fval());
                    // Output curves of the best-fit multipoles if requested.
                    if(curvesOut) {
                        // A generic correlation model does not know how to calculate multipoles, so
                        // we must dynamically cast to our more specialized BAO model type.
                        boost::shared_ptr<const baofit::BaoCorrelationModel>
                            baoModel(boost::dynamic_pointer_cast<const baofit::BaoCorrelationModel>(model));
                        boost::format fmt(" %.3e %.3e %.3e");
                        double dr(1); // Mpc/h
                        int nr = 1+std::floor((rmax-rmin)/dr);
                        for(int i = 0; i < nr; ++i) {
                            double r = rmin + i*dr;
                            std::vector<double> xi = baoModel->evaluateMultipoles(r,params);
                            *curvesOut << fmt % xi[0] % xi[1] % xi[2];
                        }
                        *curvesOut << std::endl;
                    }
                }
                else {
                    nInvalid++;
                }
                if(verbose && (0 == (k+1)%10)) {
                    std::cout << "Completed " << (k+1) << " bootstrap trials (" << nInvalid
                        << " invalid)" << std::endl;
                }
            }
            if(curvesOut) curvesOut->close();
            out.close();
            for(int i = 0; i < npar; ++i) {
                std::cout << i << ' ' << accumulators[i].mean() << " +/- "
                    << accumulators[i].error() << "\t\t[ " << bestParams[i] << " +/- "
                    << bestErrors[i] << " ]" << std::endl;
            }
            std::cout << "minChiSq = " << accumulators[npar].mean() << " +/- "
                << accumulators[npar].error() << "\t\t[ " << bestFval << " ]" << std::endl;
        }
        
        if(dumpName.length() > 0) {
            if(verbose) std::cout << "Dumping fit results to " << dumpName << std::endl;
            //nll.dump(dumpName,fmin.UserParameters().Params(),contourData,modelBins);
        }
    }
    catch(cosmo::RuntimeError const &e) {
        std::cerr << "ERROR during fit:\n  " << e.what() << std::endl;
        return -2;
    }
    catch(lk::RuntimeError const &e) {
        std::cerr << "ERROR during fit:\n  " << e.what() << std::endl;
        return -2;
    }

    // All done: normal exit.
    return 0;
}