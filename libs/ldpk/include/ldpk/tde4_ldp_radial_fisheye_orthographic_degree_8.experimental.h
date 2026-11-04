// This file is part of the Lens Distortion Plugin Kit
// Software is provided "as is" - no warranties implied.
// (C) 2011 - 2024,  Science-D-Visions. Current version: 2.12.0

#pragma once

#include <ldpk/tde4_ldp_radial_fisheye_base_degree_8.experimental.h>

//! @file tde4_ldp_radial_fisheye_orthographic_degree_8.h
//! @brief Plugin class for radial distortion

template <class VEC2,class MAT2>
class tde4_ldp_radial_fisheye_orthographic_degree_8:public tde4_ldp_radial_fisheye_base_degree_8<VEC2,MAT2,ldpk::mapping_function_orthographic<VEC2,MAT2> >
	{
private:
	typedef tde4_ldp_radial_fisheye_base_degree_8<VEC2,MAT2,ldpk::mapping_function_orthographic<VEC2,MAT2> > base_type;
protected:
	bool getModelName(char *name) override
		{
#ifdef LDPK_COMPILE_AS_PLUGIN_SDV
		strcpy(name,"3DE4 Radial - Fisheye, Orthographic, Degree 8 [Plugin]");
#else
		strcpy(name,"3DE4 Radial - Fisheye, Orthographic, Degree 8");
#endif
		return true;
		}
	};
