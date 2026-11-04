// This file is part of the Lens Distortion Plugin Kit
// Software is provided "as is" - no warranties implied.
// (C) 2011 - 2024,  Science-D-Visions. Current version: 2.12.0

#pragma once

//! @file ldpk_generic_distortion_base.h
//! @brief Base class for distortion models

#include <ldpk/ldpk_error.h>
#include <cmath>
#include <functional>
#include <iostream>
#include <vector>

namespace ldpk
	{
//! @brief Base class for a distortion model with N parameters.
//! You may find it useful to derive your own distortion model class from this one.
//! It contains methods for inverting the distortion model function.
//! We derive this class from the standard unary function class
//! in order to get a well-defined function object.
	template <class VEC2,class MAT2,int N>
	class generic_distortion_base
		{
	private:
		typedef VEC2 vec2_type;
		typedef MAT2 mat2_type;

// For the inverse mapping we have termination conditions
		double _epsilon;
// After n_max_iter we give up. This should not happen.
		int _n_max_iter;
// n_post_iter is the number of iterations we perform after
// epsilon is reached.
		int _n_post_iter;

	protected:
//! A derived class may check if the index is valid.
		void check_range(int i) const
			{
			if((i < 0) || (i >= N))
				{ throw error_index_out_of_range(i,0,N - 1); }
			}
	public:
		generic_distortion_base()
			{
			_epsilon = 1e-6;
			_n_max_iter = 20;
			_n_post_iter = 2;
			}
//! Number of parameters, that is N.
		int get_num_parameters() const
			{ return N; }
//! @brief Configure iterative procedure for map_inverse(). Call this, if you don't agree
//! with the default values.
		void setup_map_inverse(int n_max_iter,int n_post_iter,double epsilon)
			{
			_n_max_iter = n_max_iter;
			_n_post_iter = n_post_iter;
			_epsilon = epsilon;
			}
//! @brief After changing one or more coefficients of the model, call this (future use).
//! The derived class may prepare some data structure for fast/precise evalutation..
		virtual void done()
			{ }
//! @brief There must be methods to address coefficients by one single index i in [0,N[
		virtual double get_coeff(int i) const = 0;
		virtual void set_coeff(int i,double) = 0;
//! @brief User-defined maximum number of iterations applied in map_inverse in order to
//! fulfill the termination condition.
		int get_n_max_iter() const
			{ return _n_max_iter; }
//! @brief User-defined number of additional iterations to be applied when
//! the termination condition is fulfilled (which we call post-iterations).
		int get_n_post_iter() const
			{ return _n_post_iter; }
//! @brief Remove distortion. This method is non-iterative.
		virtual vec2_type operator()(const vec2_type& p) const = 0;
//! @brief Same as method instead of operator
		vec2_type eval(const vec2_type& p) const
			{ return (*this)(p); }
//! @brief Jacobi-Matrix. The result is a matrix g_{ij} = d/dp_j f(p)_i, where f represents the undistort-function.
//! We compute this by means of difference quotients. This requires four evaluations. For better performance,
//! you can implement the analytic form in your derived distortion class.
		virtual mat2_type jacobi(const vec2_type& p_dn) const
			{
			const double epsilon = 1e-4;
			return	trans(mat2_type((eval(p_dn + vec2_type(epsilon,0)) - eval(p_dn - vec2_type(epsilon,0))) / (2.0 * epsilon),
						(eval(p_dn + vec2_type(0,epsilon)) - eval(p_dn - vec2_type(0,epsilon))) / (2.0 * epsilon)));
			}
//! Not all distortion functions will support this
		virtual void derive(double* dg,int n_parameters,const vec2_type& p_dn) const
			{
			std::cout << "ldpk::generic_distortion_base::derive: not implemented" << std::endl;
			}
//! @brief Inverse mapping by solving the fixed point equation without providing initial values.
//! Virtual, because the derived class might use some smart data structure for calculating an initial value.
		 virtual vec2_type map_inverse(const vec2_type& q) const
			{
			vec2_type p = q - ((*this)(q) - q);
// Iterate until |f(p_iter) - q| < epsilon.
			for(int i = 0;i < _n_max_iter;++i)
				{
				vec2_type q_iter = (*this)(p);
				p = p + q - q_iter;
				double diff = norm2(q_iter - q);
				if(diff < _epsilon)
					{ break; }
				}
// We improve precision by a fixed number of post-iterations.
			for(int i = 0;i < _n_post_iter;++i)
				{
				vec2_type q_iter = (*this)(p);
				p = p + q - q_iter;
				}
			return p;
			}
//! @brief For given q, we are looking for p so that f(p) = q. p_start is near to p.
		vec2_type map_inverse(const vec2_type& q,const vec2_type& p_start) const
			{
			vec2_type p = p_start;
// In case of no problems there is no need for relaxation,
// but there are case which require relaxation.
			double relax = 1.0;
// Also in case of trouble we will need more iterations.
			int n_iter = _n_max_iter;

static bool done_initial_value_method = false;
static tde4_initial_value_method_enum initial_value_method = tde4_initial_lut;
static double relax_by_envvar = 1.0;
static bool message_issued = false;
if(!done_initial_value_method)
	{
	if(!::getenv("TDE4_LDPK_INITIAL_VALUE_METHOD"))
		{ initial_value_method = tde4_initial_lut; }
	else if(strcmp(::getenv("TDE4_LDPK_INITIAL_VALUE_METHOD"),"lut") == 0)
		{ initial_value_method = tde4_initial_lut; }
	else if(strcmp(::getenv("TDE4_LDPK_INITIAL_VALUE_METHOD"),"heuristic") == 0)
		{ initial_value_method = tde4_initial_heuristic; }
	else if(strcmp(::getenv("TDE4_LDPK_INITIAL_VALUE_METHOD"),"heuristic_relax") == 0)
		{
		initial_value_method = tde4_initial_heuristic_relax;
		if(::getenv("TDE4_LDPK_INITIAL_VALUE_RELAX"))
			{ relax_by_envvar = ::atof(::getenv("TDE4_LDPK_INITIAL_VALUE_RELAX")); }
		else
			{ std::cerr << "envvar TDE4_LDPK_INITIAL_VALUE_RELAX not found" << std::endl; }
		}
	else
		{
		std::cerr << "illegal value for envvar TDE4_LDPK_INITIAL_VALUE_METHOD" << std::endl;
		initial_value_method = tde4_initial_lut;
		}
	done_initial_value_method = true;
	}
switch(initial_value_method)
	{
	case tde4_initial_lut:
		{
		break;
		}
	case tde4_initial_heuristic:
		{
		if(norm2(q) > 1.0)
			{ p = unit(q); }
		else
			{ p = q; }
		relax = 1.0;
		n_iter = 50;
		break;
		}
	case tde4_initial_heuristic_relax:
		{
		if(norm2(q) > 1.0)
			{ p = unit(q); }
		else
			{ p = q; }
		relax = relax_by_envvar;
		n_iter = 50;
		break;
		}
	}
if(!message_issued)
	{
	std::cerr << "set relax to " << relax  << std::endl;
	std::cerr << "set n_iter to " << n_iter  << std::endl;
	message_issued = true;
	}

			for(int i = 0;i < n_iter;++i)
				{
// Compute Jacobian for updated point p.
				mat2_type g = jacobi(p);
// A Newton iteration
				vec2_type q_cmp(eval(p));
				try
					{
					p += relax * invert(g) * (q - q_cmp);
					}
				catch(...)
					{
					return vec2_type();
					}
				if(norm2(q - q_cmp) < 1e-8)
					{
					break;
					}
				}
			return p;
			}
//! @brief The derived class implements a method for printing values
//! inside 3DE4's matrix tool dialog. This functionality
//! is currently not supported by tde4_ld_plugin.
		virtual std::ostream& out(std::ostream& cout) const
			{ return cout; }
		};
	}

