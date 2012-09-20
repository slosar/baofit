// Created 24-May-2012 by David Kirkby (University of California, Irvine) <dkirkby@uci.edu>

#ifndef BAOFIT_QUASAR_CORRELATION_DATA
#define BAOFIT_QUASAR_CORRELATION_DATA

#include "baofit/AbsCorrelationData.h"

#include "cosmo/types.h"

namespace baofit {
	class QuasarCorrelationData : public AbsCorrelationData {
	// Represents a quasar transmission-fraction (F) correlation function binned in observed
	// coordinates log(lambda2/lambda1), angular separation between lines of sight, and
	// average absorption redshift.
	public:
	    // Creates a new object using the specified binning and cosmology to map the observed coordinates
	    // into co-moving coordinates. The data will be pruned to rmin <= r < rmax (in Mpc/h) and
	    // log(lambda2/lambda1) > llmin when the finalize() method is called.
		QuasarCorrelationData(likely::AbsBinningCPtr axis1, likely::AbsBinningCPtr axis2,
		    likely::AbsBinningCPtr axis3, double llmin, bool fixCov,
			cosmo::AbsHomogeneousUniversePtr cosmology);
        QuasarCorrelationData(std::vector<likely::AbsBinningCPtr> axes, double llmin,
            bool fixCov, cosmo::AbsHomogeneousUniversePtr cosmology);
		virtual ~QuasarCorrelationData();
		// Polymorphic shallow copy so this type of data can be used with likely::BinnedDataResampler.
        virtual QuasarCorrelationData *clone(bool binningOnly = false) const;
        // Returns the 3D radius in Mpc/h associated with the specified global index.
        virtual double getRadius(int index) const;
        // Returns the cosine of the angle between the separation vector and
        // the line of sight (aka mu) associated with the specified global index.
        virtual double getCosAngle(int index) const;
        // Returns the redshift associated with the specified global index.
	virtual double getRedshift(int index) const;
    	// This fixes covariance by adding the correct terms for a typical BAO analysis
    	// that throw away unwanted modes spuriosly appearing
	// (for not yet completelly understood
    	// reasons). The covariance between bins at the same redshift and separation, and
    	// log-lambda values ll1,ll2 is increased by a k=0 power and flat power bins in k:
	// (0,k1) and (k1,k2)
	void fixCovariance(double k1=150., double k2=300., double c=1e-3);
        // Finalize a quasar dataset by pruning to the limits specified in our constructor, optionally
        // fixing covariance and tabulating the co-moving coordinates at the center of each remaining
        // bin with data.
        // No further changes to our "shape" are possible after finalizing. See the documentation
        // for BinnedData::finalize() for details.
        virtual void finalize();
        // Transforms the specified values of ll,sep,dsep,z to co-moving r,mu.
        void transform(double ll, double sep, double dsep, double z, double &r, double &mu) const;
	private:
        void _initialize(double llmin, bool fixCov, cosmo::AbsHomogeneousUniversePtr cosmology);
	// This produces contribution to the 1D xi from a power spectrum bin between kmin and kmax. 
	double _pkmarg(double kmin, double kmax, double l1, double l2);
        double _llmin;
    	bool _fixCov;
        cosmo::AbsHomogeneousUniversePtr _cosmology;
        std::vector<double> _rLookup, _muLookup, _zLookup;
        // Calculates and saves (r,mu,z) for the specified global index.
        void _setIndex(int index) const;
        mutable int _lastIndex;
        mutable std::vector<double> _binCenter,_binWidth;
        mutable double _rLast, _muLast, _zLast;
        double _arcminToRad;
	}; // QuasarCorrelationData
} // baofit

#endif // BAOFIT_QUASAR_CORRELATION_DATA
