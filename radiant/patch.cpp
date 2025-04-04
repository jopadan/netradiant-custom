/*
   Copyright (C) 2001-2006, William Joseph.
   All Rights Reserved.

   This file is part of GtkRadiant.

   GtkRadiant is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   GtkRadiant is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GtkRadiant; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#define _USE_MATH_DEFINES

#include "patch.h"

#include <forward_list>
#include "preferences.h"
#include "brush_primit.h"
#include "signal/signal.h"


Signal0 g_patchTextureChangedCallbacks;

void Patch_addTextureChangedCallback( const SignalHandler& handler ){
	g_patchTextureChangedCallbacks.connectLast( handler );
}

void Patch_textureChanged(){
	g_patchTextureChangedCallbacks();
}


Counter* PatchInstance::m_counter = 0;
Shader* PatchInstance::m_state_selpoint;
Shader* Patch::m_state_ctrl;
Shader* Patch::m_state_lattice;
EPatchType Patch::m_type;


std::size_t MAX_PATCH_WIDTH = 0;
std::size_t MAX_PATCH_HEIGHT = 0;

int g_PatchSubdivideThreshold = 4;

void BezierCurveTree_Delete( BezierCurveTree *pCurve ){
	if ( pCurve ) {
		BezierCurveTree_Delete( pCurve->left );
		BezierCurveTree_Delete( pCurve->right );
		delete pCurve;
	}
}

std::size_t BezierCurveTree_Setup( BezierCurveTree *pCurve, std::size_t index, std::size_t stride ){
	if ( pCurve ) {
		if ( pCurve->left && pCurve->right ) {
			index = BezierCurveTree_Setup( pCurve->left, index, stride );
			pCurve->index = index * stride;
			index++;
			index = BezierCurveTree_Setup( pCurve->right, index, stride );
		}
		else
		{
			pCurve->index = BEZIERCURVETREE_MAX_INDEX;
		}
	}

	return index;
}

bool BezierCurve_IsCurved( const BezierCurve& curve ){
	Vector3 vTemp( vector3_subtracted( curve.right, curve.left ) );
	Vector3 v1( vector3_subtracted( curve.crd, curve.left ) );
	Vector3 v2( vector3_subtracted( curve.right, curve.crd ) );

	if ( vector3_equal( v1, g_vector3_identity ) || vector3_equal( vTemp, v1 ) ) { // return 0 if 1->2 == 0 or 1->2 == 1->3
		return false;
	}

	vector3_normalise( v1 );
	vector3_normalise( v2 );
	if ( vector3_equal( v1, v2 ) ) {
		return false;
	}

	Vector3 v3( vTemp );
	const double width = vector3_length( v3 );
	vector3_scale( v3, 1.0 / width );

	if ( vector3_equal( v1, v3 ) && vector3_equal( v2, v3 ) ) {
		return false;
	}

	const double angle = acos( vector3_dot( v1, v2 ) ) / c_pi;

	const double index = width * angle;

	if ( index > static_cast<double>( g_PatchSubdivideThreshold ) ) {
		return true;
	}
	return false;
}

void BezierInterpolate( BezierCurve& curve ){
	curve.left = vector3_mid( curve.left, curve.crd );
	curve.right = vector3_mid( curve.crd, curve.right );
	curve.crd = vector3_mid( curve.left, curve.right );
}

const std::size_t PATCH_MAX_SUBDIVISION_DEPTH = 16;

void BezierCurveTree_FromCurveList( BezierCurveTree *pTree, std::forward_list<BezierCurve>& curveList, std::size_t depth = 0 ){
	std::forward_list<BezierCurve> leftList;
	std::forward_list<BezierCurve> rightList;
	bool bSplit = false;

	for ( BezierCurve& curve : curveList )
	{
		if ( bSplit || BezierCurve_IsCurved( curve ) ) {
			bSplit = true;
			BezierCurve& leftCurve = leftList.emplace_front();
			BezierCurve& rightCurve = rightList.emplace_front();
			leftCurve.left = curve.left;
			rightCurve.right = curve.right;
			BezierInterpolate( curve );
			leftCurve.crd = curve.left;
			rightCurve.crd = curve.right;
			leftCurve.right = curve.crd;
			rightCurve.left = curve.crd;
		}
	}

	if ( !leftList.empty() && !rightList.empty() && depth != PATCH_MAX_SUBDIVISION_DEPTH ) {
		pTree->left = new BezierCurveTree;
		pTree->right = new BezierCurveTree;
		BezierCurveTree_FromCurveList( pTree->left, leftList, depth + 1 );
		BezierCurveTree_FromCurveList( pTree->right, rightList, depth + 1 );
	}
	else
	{
		pTree->left = 0;
		pTree->right = 0;
	}
}


void Patch::setDims( std::size_t w, std::size_t h ){
	if ( ( w % 2 ) == 0 ) {
		w -= 1;
	}
	ASSERT_MESSAGE( w <= MAX_PATCH_WIDTH, "patch too wide" );
	if ( w > MAX_PATCH_WIDTH ) {
		w = MAX_PATCH_WIDTH;
	}
	else if ( w < MIN_PATCH_WIDTH ) {
		w = MIN_PATCH_WIDTH;
	}

	if ( ( h % 2 ) == 0 ) {
		m_height -= 1;
	}
	ASSERT_MESSAGE( h <= MAX_PATCH_HEIGHT, "patch too tall" );
	if ( h > MAX_PATCH_HEIGHT ) {
		h = MAX_PATCH_HEIGHT;
	}
	else if ( h < MIN_PATCH_HEIGHT ) {
		h = MIN_PATCH_HEIGHT;
	}

	m_width = w;
	m_height = h;

	if ( m_width * m_height != m_ctrl.size() ) {
		m_ctrl.resize( m_width * m_height );
		onAllocate( m_ctrl.size() );
	}
}

inline const Colour4b& colour_for_index( std::size_t i, std::size_t width ){
	return ( i % 2 || ( i / width ) % 2 ) ? colour_inside : colour_corner;
}

inline bool float_valid( float f ){
	return f == f;
}

bool Patch::isValid() const {
	if ( !m_width || !m_height ) {
		return false;
	}

	for ( const_iterator i = m_ctrl.begin(); i != m_ctrl.end(); ++i )
	{
		if ( !float_valid( ( *i ).m_vertex.x() )
		  || !float_valid( ( *i ).m_vertex.y() )
		  || !float_valid( ( *i ).m_vertex.z() )
		  || !float_valid( ( *i ).m_texcoord.x() )
		  || !float_valid( ( *i ).m_texcoord.y() ) ) {
			globalErrorStream() << "patch has invalid control points\n";
			return false;
		}
	}
	return true;
}

void Patch::UpdateCachedData(){
	m_ctrl_vertices.clear();
	m_lattice_indices.clear();

	if ( !isValid() ) {
		m_tess.m_numStrips = 0;
		m_tess.m_lenStrips = 0;
		m_tess.m_nArrayHeight = 0;
		m_tess.m_nArrayWidth = 0;
		m_tess.m_curveTreeU.resize( 0 );
		m_tess.m_curveTreeV.resize( 0 );
		m_tess.m_indices.resize( 0 );
		m_tess.m_vertices.resize( 0 );
		m_tess.m_arrayHeight.resize( 0 );
		m_tess.m_arrayWidth.resize( 0 );
		m_aabb_local = AABB();
		return;
	}

	BuildTesselationCurves( ROW );
	BuildTesselationCurves( COL );
	BuildVertexArray();
	AccumulateBBox();

	IndexBuffer ctrl_indices;

	m_lattice_indices.reserve( ( ( m_width * ( m_height - 1 ) ) + ( m_height * ( m_width - 1 ) ) ) << 1 );
	ctrl_indices.reserve( m_ctrlTransformed.size() );
	{
		UniqueVertexBuffer<PointVertex> inserter( m_ctrl_vertices );
		for ( iterator i = m_ctrlTransformed.begin(); i != m_ctrlTransformed.end(); ++i )
		{
			ctrl_indices.insert( inserter.insert( pointvertex_quantised( PointVertex( reinterpret_cast<const Vertex3f&>( ( *i ).m_vertex ), colour_for_index( i - m_ctrlTransformed.begin(), m_width ) ) ) ) );
		}
	}
	{
		for ( IndexBuffer::iterator i = ctrl_indices.begin(); i != ctrl_indices.end(); ++i )
		{
			if ( std::size_t( i - ctrl_indices.begin() ) % m_width ) {
				m_lattice_indices.insert( *( i - 1 ) );
				m_lattice_indices.insert( *i );
			}
			if ( std::size_t( i - ctrl_indices.begin() ) >= m_width ) {
				m_lattice_indices.insert( *( i - m_width ) );
				m_lattice_indices.insert( *i );
			}
		}
	}

#if 0
	{
		Array<RenderIndex>::iterator first = m_tess.m_indices.begin();
		for ( std::size_t s = 0; s < m_tess.m_numStrips; s++ )
		{
			Array<RenderIndex>::iterator last = first + m_tess.m_lenStrips;

			for ( Array<RenderIndex>::iterator i( first ); i + 2 != last; i += 2 )
			{
				ArbitraryMeshTriangle_sumTangents( m_tess.m_vertices[*( i + 0 )], m_tess.m_vertices[*( i + 1 )], m_tess.m_vertices[*( i + 2 )] );
				ArbitraryMeshTriangle_sumTangents( m_tess.m_vertices[*( i + 2 )], m_tess.m_vertices[*( i + 1 )], m_tess.m_vertices[*( i + 3 )] );
			}

			first = last;
		}

		for ( Array<ArbitraryMeshVertex>::iterator i = m_tess.m_vertices.begin(); i != m_tess.m_vertices.end(); ++i )
		{
			vector3_normalise( reinterpret_cast<Vector3&>( ( *i ).tangent ) );
			vector3_normalise( reinterpret_cast<Vector3&>( ( *i ).bitangent ) );
		}
	}
#endif

	if( !m_transformChanged ) //experimental! fixing extra sceneChangeNotify call during scene rendering
		SceneChangeNotify();
}

void Patch::InvertMatrix(){
	undoSave();

	PatchControlArray_invert( m_ctrl, m_width, m_height );

	controlPointsChanged();
}

void Patch::TransposeMatrix(){
	undoSave();

	{
		Array<PatchControl> tmp( m_width * m_height );
		copy_ctrl( tmp.data(), m_ctrl.data(), m_ctrl.data() + m_width * m_height );

		PatchControlIter from = tmp.data();
		for ( std::size_t h = 0; h != m_height; ++h )
		{
			PatchControlIter to = m_ctrl.data() + h;
			for ( std::size_t w = 0; w != m_width; ++w, ++from, to += m_height )
			{
				*to = *from;
			}
		}
	}

	std::swap( m_width, m_height );

	controlPointsChanged();
}

void Patch::Redisperse( EMatrixMajor mt ){
	std::size_t w, h, width, height, row_stride, col_stride;
	PatchControl* p1, * p2, * p3;

	undoSave();

	switch ( mt )
	{
	case COL:
		width = ( m_width - 1 ) >> 1;
		height = m_height;
		col_stride = 1;
		row_stride = m_width;
		break;
	case ROW:
		width = ( m_height - 1 ) >> 1;
		height = m_width;
		col_stride = m_width;
		row_stride = 1;
		break;
	default:
		ERROR_MESSAGE( "neither row-major nor column-major" );
		return;
	}

	for ( h = 0; h < height; h++ )
	{
		p1 = m_ctrl.data() + ( h * row_stride );
		for ( w = 0; w < width; w++ )
		{
			p2 = p1 + col_stride;
			p3 = p2 + col_stride;
			p2->m_vertex = vector3_mid( p1->m_vertex, p3->m_vertex );
			p1 = p3;
		}
	}

	controlPointsChanged();
}

void Patch::Smooth( EMatrixMajor mt ){
	std::size_t w, h, width, height, row_stride, col_stride;
	bool wrap;
	PatchControl* p1, * p2, * p3, * p2b;

	undoSave();

	switch ( mt )
	{
	case COL:
		width = ( m_width - 1 ) >> 1;
		height = m_height;
		col_stride = 1;
		row_stride = m_width;
		break;
	case ROW:
		width = ( m_height - 1 ) >> 1;
		height = m_width;
		col_stride = m_width;
		row_stride = 1;
		break;
	default:
		ERROR_MESSAGE( "neither row-major nor column-major" );
		return;
	}

	wrap = true;
	for ( h = 0; h < height; h++ )
	{
		p1 = m_ctrl.data() + ( h * row_stride );
		p2 = p1 + ( 2 * width ) * col_stride;
		//globalErrorStream() << "compare " << p1->m_vertex << " and " << p2->m_vertex << '\n';
		if ( vector3_length_squared( vector3_subtracted( p1->m_vertex, p2->m_vertex ) ) > 1.0 ) {
			//globalErrorStream() << "too far\n";
			wrap = false;
			break;
		}
	}

	for ( h = 0; h < height; h++ )
	{
		p1 = m_ctrl.data() + ( h * row_stride ) + col_stride;
		for ( w = 0; w < width - 1; w++ )
		{
			p2 = p1 + col_stride;
			p3 = p2 + col_stride;
			p2->m_vertex = vector3_mid( p1->m_vertex, p3->m_vertex );
			p1 = p3;
		}
		if ( wrap ) {
			p1 = m_ctrl.data() + ( h * row_stride ) + ( 2 * width - 1 ) * col_stride;
			p2 = m_ctrl.data() + ( h * row_stride );
			p2b = m_ctrl.data() + ( h * row_stride ) + ( 2 * width ) * col_stride;
			p3 = m_ctrl.data() + ( h * row_stride ) + col_stride;
			p2->m_vertex = p2b->m_vertex = vector3_mid( p1->m_vertex, p3->m_vertex );
		}
	}

	controlPointsChanged();
}

void Patch::InsertRemove( bool bInsert, bool bColumn, bool bFirst ){
	undoSave();

	if ( bInsert ) {
		if ( bColumn && ( m_width + 2 <= MAX_PATCH_WIDTH ) ) {
			InsertPoints( COL, bFirst );
		}
		else if ( m_height + 2 <= MAX_PATCH_HEIGHT ) {
			InsertPoints( ROW, bFirst );
		}
	}
	else
	{
		if ( bColumn && ( m_width - 2 >= MIN_PATCH_WIDTH ) ) {
			RemovePoints( COL, bFirst );
		}
		else if ( m_height - 2 >= MIN_PATCH_HEIGHT ) {
			RemovePoints( ROW, bFirst );
		}
	}

	controlPointsChanged();
}

Patch* Patch::MakeCap( Patch* patch, EPatchCap eType, EMatrixMajor mt, bool bFirst ){
	std::size_t i, width, height;

	switch ( mt )
	{
	case ROW:
		width = m_width;
		height = m_height;
		break;
	case COL:
		width = m_height;
		height = m_width;
		break;
	default:
		ERROR_MESSAGE( "neither row-major nor column-major" );
		return 0;
	}

	Array<Vector3> p( width );

	std::size_t nIndex = ( bFirst ) ? 0 : height - 1;
	if ( mt == ROW ) {
		for ( i = 0; i < width; i++ )
		{
			p[( bFirst ) ? i : ( width - 1 ) - i] = ctrlAt( nIndex, i ).m_vertex;
		}
	}
	else
	{
		for ( i = 0; i < width; i++ )
		{
			p[( bFirst ) ? i : ( width - 1 ) - i] = ctrlAt( i, nIndex ).m_vertex;
		}
	}

	patch->ConstructSeam( eType, p.data(), width );
	return patch;
}

void Patch::FlipTexture( int nAxis ){
	undoSave();

	for ( PatchControlIter i = m_ctrl.data(); i != m_ctrl.data() + m_ctrl.size(); ++i )
	{
		( *i ).m_texcoord[nAxis] = -( *i ).m_texcoord[nAxis];
	}

	controlPointsChanged();
	Patch_textureChanged();
}

void Patch::TranslateTexture( float s, float t ){
	undoSave();

	s = -1 * s / m_state->getTexture().width;
	t = t / m_state->getTexture().height;

	for ( PatchControlIter i = m_ctrl.data(); i != m_ctrl.data() + m_ctrl.size(); ++i )
	{
		( *i ).m_texcoord[0] += s;
		( *i ).m_texcoord[1] += t;
	}

	controlPointsChanged();
	Patch_textureChanged();
}

void Patch::ScaleTexture( float s, float t ){
	undoSave();

	for ( PatchControlIter i = m_ctrl.data(); i != m_ctrl.data() + m_ctrl.size(); ++i )
	{
		( *i ).m_texcoord[0] *= s;
		( *i ).m_texcoord[1] *= t;
	}

	controlPointsChanged();
	Patch_textureChanged();
}

void Patch::RotateTexture( float angle ){
	undoSave();

	const float s = static_cast<float>( sin( degrees_to_radians( angle ) ) );
	const float c = static_cast<float>( cos( degrees_to_radians( angle ) ) );

	for ( PatchControlIter i = m_ctrl.data(); i != m_ctrl.data() + m_ctrl.size(); ++i )
	{
		const float x = ( *i ).m_texcoord[0];
		const float y = ( *i ).m_texcoord[1];
		( *i ).m_texcoord[0] = ( x * c ) - ( y * s );
		( *i ).m_texcoord[1] = ( y * c ) + ( x * s );
	}

	controlPointsChanged();
	Patch_textureChanged();
}


void Patch::SetTextureRepeat( float s, float t ){
	undoSave();

	std::size_t w, h;
	float sc, tc;

	const float si = ( s == 0? 1.f : s ) / ( m_width - 1 );
	const float ti = ( t == 0? 1.f : t ) / ( m_height - 1 );

	PatchControl *pDest = m_ctrl.data();
	for ( h = 0, tc = 0.0f; h < m_height; h++, tc += ti )
	{
		for ( w = 0, sc = 0.0f; w < m_width; w++, sc += si )
		{
			pDest->m_texcoord[0] = sc;
			pDest->m_texcoord[1] = tc;
			pDest++;
		}
	}

	controlPointsChanged();
	Patch_textureChanged();
}

/*
	void Patch::SetTextureInfo( texdef_t *pt )
	{
	if( pt->getShift()[0] || pt->getShift()[1] )
		TranslateTexture( pt->getShift()[0], pt->getShift()[1] );
	else if( pt->getScale()[0] || pt->getScale()[1] )
	{
		if( pt->getScale()[0] == 0.0f ) pt->setScale( 0, 1.0f );
		if( pt->getScale()[1] == 0.0f ) pt->setScale( 1, 1.0f );
		ScaleTexture ( pt->getScale()[0], pt->getScale()[1] );
	}
	else if( pt->rotate )
		RotateTexture( pt->rotate );
	}
 */

void Patch::Calculate_AvgAxes( Vector3& wDir, Vector3& hDir ) const {
	wDir = hDir = g_vector3_identity;
	for ( std::size_t r = 0; r < m_height; ++r ){
		wDir += ctrlAt( r, m_width - 1 ).m_vertex - ctrlAt( r, 0 ).m_vertex;
	}
	for ( std::size_t c = 0; c < m_width; ++c ){
		hDir += ctrlAt( m_height - 1, c ).m_vertex - ctrlAt( 0, c ).m_vertex;
	}
	/* fallback to longest hord */
	if ( vector3_equal_epsilon( wDir, g_vector3_identity, 1.f ) ){
		float bestLength = 0;
		for ( std::size_t r = 0; r < m_height; ++r ){
			for ( std::size_t c = 0; c < m_width - 1; ++c ){
				const Vector3 dir = ctrlAt( r, c + 1 ).m_vertex - ctrlAt( r, c ).m_vertex;
				const float length = vector3_length( dir );
				if ( length - bestLength > 1 ){
					bestLength = length;
					wDir = dir;
				}
			}
		}
	}
	if( vector3_equal_epsilon( hDir, g_vector3_identity, 1.f ) ){
		float bestLength = 0;
		for ( std::size_t c = 0; c < m_width; ++c ){
			for ( std::size_t r = 0; r < m_height - 1; ++r ){
				const Vector3 dir = ctrlAt( r + 1, c ).m_vertex - ctrlAt( r, c ).m_vertex;
				const float length = vector3_length( dir );
				if ( length - bestLength > 1 ){
					bestLength = length;
					hDir = dir;
				}
			}
		}
	}

	if ( vector3_equal_epsilon( vector3_cross( wDir, hDir ), g_vector3_identity, 0.1f ) ) {
		wDir = g_vector3_axis_x;
		hDir = g_vector3_axis_y;
	}
}

Vector3 Patch::Calculate_AvgNormal() const {
	Vector3 wDir, hDir;
	Calculate_AvgAxes( wDir, hDir );
	return vector3_normalised( vector3_cross( wDir, hDir ) );
}

inline int texture_axis( const Vector3& normal ){
	// axis dominance order: Z, X, Y
	return ( normal.x() >= normal.y() )
	       ? ( normal.x() > normal.z() )
	         ? 0
	         : 2
	       : ( normal.y() > normal.z() )
	         ? 1
	         : 2;
}

void Patch::CapTexture(){
#if 0
	const PatchControl& p1 = m_ctrl[m_width];
	const PatchControl& p2 = m_ctrl[m_width * ( m_height - 1 )];
	const PatchControl& p3 = m_ctrl[( m_width * m_height ) - 1];


	Vector3 normal( g_vector3_identity );

	{
		Vector3 tmp( vector3_cross(
		                 vector3_subtracted( p2.m_vertex, m_ctrl[0].m_vertex ),
		                 vector3_subtracted( p3.m_vertex, m_ctrl[0].m_vertex )
		             ) );
		if ( !vector3_equal( tmp, g_vector3_identity ) ) {
			vector3_add( normal, tmp );
		}
	}
	{
		Vector3 tmp( vector3_cross(
		                 vector3_subtracted( p1.m_vertex, p3.m_vertex ),
		                 vector3_subtracted( m_ctrl[0].m_vertex, p3.m_vertex )
		             ) );
		if ( !vector3_equal( tmp, g_vector3_identity ) ) {
			vector3_add( normal, tmp );
		}
	}
	normal[0] = fabs( normal[0] );
	normal[1] = fabs( normal[1] );
	normal[2] = fabs( normal[2] );

	ProjectTexture( texture_axis( normal ) );
#else
	Vector3 normal = Calculate_AvgNormal();
	TextureProjection projection;
	TexDef_Construct_Default( projection );
	ComputeAxisBase( normal, projection.m_basis_s, projection.m_basis_t ); /* Valve220 */
	ProjectTexture( projection, normal );
#endif
}

// uses longest parallel chord to calculate texture coords for each row/col
void Patch::NaturalTexture(){
	undoSave();

	{
		const double texSize = (double)m_state->getTexture().width * Texdef_getDefaultTextureScale();

		double texBest = 0;
		double tex = 0;
		PatchControl* pWidth = m_ctrl.data();
		for ( std::size_t w = 0; w < m_width; ++w, ++pWidth )
		{
			{
				PatchControl* pHeight = pWidth;
				for ( std::size_t h = 0; h < m_height; ++h, pHeight += m_width )
					pHeight->m_texcoord[0] = static_cast<float>( tex );
			}

			if ( w + 1 == m_width ) {
				break;
			}

			{
				const PatchControl* pHeight = pWidth;
				for ( std::size_t h = 0; h < m_height; ++h, pHeight += m_width )
				{
					const double length = tex + ( vector3_length( pHeight->m_vertex - ( pHeight + 1 )->m_vertex ) / texSize );
					if ( fabs( length ) > fabs( texBest ) ) { // comparing abs values supports possible negative Texdef_getDefaultTextureScale()
						texBest = length;
					}
				}
			}

			tex = texBest;
		}
	}

	{
		const double texSize = -(double)m_state->getTexture().height * Texdef_getDefaultTextureScale();

		double texBest = 0;
		double tex = 0;
		PatchControl* pHeight = m_ctrl.data();
		for ( std::size_t h = 0; h < m_height; ++h, pHeight += m_width )
		{
			{
				PatchControl* pWidth = pHeight;
				for ( std::size_t w = 0; w < m_width; ++w, ++pWidth )
					pWidth->m_texcoord[1] = static_cast<float>( tex );
			}

			if ( h + 1 == m_height ) {
				break;
			}

			{
				const PatchControl* pWidth = pHeight;
				for ( std::size_t w = 0; w < m_width; ++w, ++pWidth )
				{
					const double length = tex + ( vector3_length( pWidth->m_vertex - ( pWidth + m_width )->m_vertex ) / texSize );
					if ( fabs( length ) > fabs( texBest ) ) {
						texBest = length;
					}
				}
			}

			tex = texBest;
		}
	}

	controlPointsChanged();
	Patch_textureChanged();
}



// private:

void Patch::AccumulateBBox(){
	m_aabb_local = AABB();

	for ( PatchControlArray::iterator i = m_ctrlTransformed.begin(); i != m_ctrlTransformed.end(); ++i )
	{
		aabb_extend_by_point_safe( m_aabb_local, ( *i ).m_vertex );
	}

	if( !m_transformChanged ) //experimental! fixing extra sceneChangeNotify call during scene rendering
		m_boundsChanged();
	m_lightsChanged();
}

void Patch::InsertPoints( EMatrixMajor mt, bool bFirst ){
	std::size_t width, height, row_stride, col_stride;

	switch ( mt )
	{
	case ROW:
		col_stride = 1;
		row_stride = m_width;
		width = m_width;
		height = m_height;
		break;
	case COL:
		col_stride = m_width;
		row_stride = 1;
		width = m_height;
		height = m_width;
		break;
	default:
		ERROR_MESSAGE( "neither row-major nor column-major" );
		return;
	}

	std::size_t pos = 0;
	{
		PatchControl* p1 = m_ctrl.data();
		/*
			if( GlobalSelectionSystem().countSelected() != 0 )
			{
				scene::Instance& instance = GlobalSelectionSystem().ultimateSelected();
				PatchInstance* patch = Instance_getPatch( instance );
				patch->m_selectable.isSelected();
			}
		*/
		for ( std::size_t w = 0; w != width; ++w, p1 += col_stride )
		{
			{
				PatchControl* p2 = p1;
				for ( std::size_t h = 1; h < height; h += 2, p2 += 2 * row_stride )
				{
					if ( 0 ) { //p2->m_selectable.isSelected())
						pos = h;
						break;
					}
				}
				if ( pos != 0 ) {
					break;
				}
			}

			{
				PatchControl* p2 = p1;
				for ( std::size_t h = 0; h < height; h += 2, p2 += 2 * row_stride )
				{
					if ( 0 ) { //p2->m_selectable.isSelected())
						pos = h;
						break;
					}
				}
				if ( pos != 0 ) {
					break;
				}
			}
		}
	}

	Array<PatchControl> tmp( m_ctrl );

	std::size_t row_stride2, col_stride2;
	switch ( mt )
	{
	case ROW:
		setDims( m_width, m_height + 2 );
		col_stride2 = 1;
		row_stride2 = m_width;
		break;
	case COL:
		setDims( m_width + 2, m_height );
		col_stride2 = m_width;
		row_stride2 = 1;
		break;
	default:
		ERROR_MESSAGE( "neither row-major nor column-major" );
		return;
	}
	if ( bFirst ) {
		pos = 2;
	}
	else
	{
		pos = height - 1;
	}

	if ( pos >= height ) {
		if ( bFirst ) {
			pos = 2;
		}
		else
		{
			pos = height - 1;
		}
	}
	else if ( pos == 0 ) {
		pos = 2;
	}
	else if ( pos % 2 ) {
		++pos;
	}


	for ( std::size_t w = 0; w != width; ++w )
	{
		PatchControl* p1 = tmp.data() + ( w * col_stride );
		PatchControl* p2 = m_ctrl.data() + ( w * col_stride2 );
		for ( std::size_t h = 0; h != height; ++h, p2 += row_stride2, p1 += row_stride )
		{
			if ( h == pos ) {
				p2 += 2 * row_stride2;
			}
			*p2 = *p1;
		}

		p1 = tmp.data() + ( w * col_stride + pos * row_stride );
		p2 = m_ctrl.data() + ( w * col_stride2 + pos * row_stride2 );

		PatchControl* r2a = ( p2 + row_stride2 );
		PatchControl* r2b = ( p2 - row_stride2 );
		PatchControl* c2a = ( p1 - 2 * row_stride );
		PatchControl* c2b = ( p1 - row_stride );

		// set two new row points
		*( p2 + 2 * row_stride2 ) = *p1;
		*r2a = *c2b;

		for ( std::size_t i = 0; i != 3; ++i )
		{
			r2a->m_vertex[i] = float_mid( c2b->m_vertex[i], p1->m_vertex[i] );

			r2b->m_vertex[i] = float_mid( c2a->m_vertex[i], c2b->m_vertex[i] );

			p2->m_vertex[i] = float_mid( r2a->m_vertex[i], r2b->m_vertex[i] );
		}
		for ( std::size_t i = 0; i != 2; ++i )
		{
			r2a->m_texcoord[i] = float_mid( c2b->m_texcoord[i], p1->m_texcoord[i] );

			r2b->m_texcoord[i] = float_mid( c2a->m_texcoord[i], c2b->m_texcoord[i] );

			p2->m_texcoord[i] = float_mid( r2a->m_texcoord[i], r2b->m_texcoord[i] );
		}
	}
}

void Patch::RemovePoints( EMatrixMajor mt, bool bFirst ){
	std::size_t width, height, row_stride, col_stride;

	switch ( mt )
	{
	case ROW:
		col_stride = 1;
		row_stride = m_width;
		width = m_width;
		height = m_height;
		break;
	case COL:
		col_stride = m_width;
		row_stride = 1;
		width = m_height;
		height = m_width;
		break;
	default:
		ERROR_MESSAGE( "neither row-major nor column-major" );
		return;
	}

	std::size_t pos = 0;
	{
		PatchControl* p1 = m_ctrl.data();
		for ( std::size_t w = 0; w != width; ++w, p1 += col_stride )
		{
			{
				PatchControl* p2 = p1;
				for ( std::size_t h = 1; h < height; h += 2, p2 += 2 * row_stride )
				{
					if ( 0 ) { //p2->m_selectable.isSelected())
						pos = h;
						break;
					}
				}
				if ( pos != 0 ) {
					break;
				}
			}

			{
				PatchControl* p2 = p1;
				for ( std::size_t h = 0; h < height; h += 2, p2 += 2 * row_stride )
				{
					if ( 0 ) { //p2->m_selectable.isSelected())
						pos = h;
						break;
					}
				}
				if ( pos != 0 ) {
					break;
				}
			}
		}
	}

	Array<PatchControl> tmp( m_ctrl );

	std::size_t row_stride2, col_stride2;
	switch ( mt )
	{
	case ROW:
		setDims( m_width, m_height - 2 );
		col_stride2 = 1;
		row_stride2 = m_width;
		break;
	case COL:
		setDims( m_width - 2, m_height );
		col_stride2 = m_width;
		row_stride2 = 1;
		break;
	default:
		ERROR_MESSAGE( "neither row-major nor column-major" );
		return;
	}
	if ( bFirst ) {
		pos = 2;
	}
	else
	{
		pos = height - 3;
	}
	if ( pos >= height ) {
		if ( bFirst ) {
			pos = 2;
		}
		else
		{
			pos = height - 3;
		}
	}
	else if ( pos == 0 ) {
		pos = 2;
	}
	else if ( pos > height - 3 ) {
		pos = height - 3;
	}
	else if ( pos % 2 ) {
		++pos;
	}

	for ( std::size_t w = 0; w != width; w++ )
	{
		PatchControl* p1 = tmp.data() + ( w * col_stride );
		PatchControl* p2 = m_ctrl.data() + ( w * col_stride2 );
		for ( std::size_t h = 0; h != height; ++h, p2 += row_stride2, p1 += row_stride )
		{
			if ( h == pos ) {
				p1 += 2 * row_stride2;
				h += 2;
			}
			*p2 = *p1;
		}

		p1 = tmp.data() + ( w * col_stride + pos * row_stride );
		p2 = m_ctrl.data() + ( w * col_stride2 + pos * row_stride2 );

		for ( std::size_t i = 0; i < 3; i++ )
		{
			( p2 - row_stride2 )->m_vertex[i] = ( ( p1 + 2 * row_stride )->m_vertex[i] + ( p1 - 2 * row_stride )->m_vertex[i] ) * 0.5f;

			( p2 - row_stride2 )->m_vertex[i] = ( p2 - row_stride2 )->m_vertex[i] + ( 2.0f * ( ( p1 )->m_vertex[i] - ( p2 - row_stride2 )->m_vertex[i] ) );
		}
		for ( std::size_t i = 0; i < 2; i++ )
		{
			( p2 - row_stride2 )->m_texcoord[i] = ( ( p1 + 2 * row_stride )->m_texcoord[i] + ( p1 - 2 * row_stride )->m_texcoord[i] ) * 0.5f;

			( p2 - row_stride2 )->m_texcoord[i] = ( p2 - row_stride2 )->m_texcoord[i] + ( 2.0f * ( ( p1 )->m_texcoord[i] - ( p2 - row_stride2 )->m_texcoord[i] ) );
		}
	}
}

void Patch::ConstructSeam( EPatchCap eType, Vector3* p, std::size_t width ){
	switch ( eType )
	{
	case EPatchCap::IBevel:
		{
			setDims( 3, 3 );
			m_ctrl[0].m_vertex = p[0];
			m_ctrl[1].m_vertex = p[1];
			m_ctrl[2].m_vertex = p[1];
			m_ctrl[3].m_vertex = p[1];
			m_ctrl[4].m_vertex = p[1];
			m_ctrl[5].m_vertex = p[1];
			m_ctrl[6].m_vertex = p[2];
			m_ctrl[7].m_vertex = p[1];
			m_ctrl[8].m_vertex = p[1];
		}
		break;
	case EPatchCap::Bevel:
		{
			setDims( 3, 3 );
			Vector3 p3( vector3_added( p[2], vector3_subtracted( p[0], p[1] ) ) );
			m_ctrl[0].m_vertex = p3;
			m_ctrl[1].m_vertex = p3;
			m_ctrl[2].m_vertex = p[2];
			m_ctrl[3].m_vertex = p3;
			m_ctrl[4].m_vertex = p3;
			m_ctrl[5].m_vertex = p[1];
			m_ctrl[6].m_vertex = p3;
			m_ctrl[7].m_vertex = p3;
			m_ctrl[8].m_vertex = p[0];
		}
		break;
	case EPatchCap::EndCap:
		{
			Vector3 p5( vector3_mid( p[0], p[4] ) );

			setDims( 3, 3 );
			m_ctrl[0].m_vertex = p[0];
			m_ctrl[1].m_vertex = p5;
			m_ctrl[2].m_vertex = p[4];
			m_ctrl[3].m_vertex = p[1];
			m_ctrl[4].m_vertex = p[2];
			m_ctrl[5].m_vertex = p[3];
			m_ctrl[6].m_vertex = p[2];
			m_ctrl[7].m_vertex = p[2];
			m_ctrl[8].m_vertex = p[2];
		}
		break;
	case EPatchCap::IEndCap:
		{
			setDims( 5, 3 );
			m_ctrl[0].m_vertex = p[4];
			m_ctrl[1].m_vertex = p[3];
			m_ctrl[2].m_vertex = p[2];
			m_ctrl[3].m_vertex = p[1];
			m_ctrl[4].m_vertex = p[0];
			m_ctrl[5].m_vertex = p[3];
			m_ctrl[6].m_vertex = p[3];
			m_ctrl[7].m_vertex = p[2];
			m_ctrl[8].m_vertex = p[1];
			m_ctrl[9].m_vertex = p[1];
			m_ctrl[10].m_vertex = p[3];
			m_ctrl[11].m_vertex = p[3];
			m_ctrl[12].m_vertex = p[2];
			m_ctrl[13].m_vertex = p[1];
			m_ctrl[14].m_vertex = p[1];
		}
		break;
	case EPatchCap::Cylinder:
		{
			std::size_t mid = ( width - 1 ) >> 1;

			bool degenerate = ( mid % 2 ) != 0;

			std::size_t newHeight = mid + ( degenerate ? 2 : 1 );

			setDims( 3, newHeight );

			if ( degenerate ) {
				++mid;
				for ( std::size_t i = width; i != width + 2; ++i )
				{
					p[i] = p[width - 1];
				}
			}

			{
				PatchControl* pCtrl = m_ctrl.data();
				for ( std::size_t i = 0; i != m_height; ++i, pCtrl += m_width )
				{
					pCtrl->m_vertex = p[i];
				}
			}
			{
				PatchControl* pCtrl = m_ctrl.data() + 2;
				std::size_t h = m_height - 1;
				for ( std::size_t i = 0; i != m_height; ++i, pCtrl += m_width )
				{
					pCtrl->m_vertex = p[h + ( h - i )];
				}
			}

			Redisperse( COL );
		}
		break;
	default:
		ERROR_MESSAGE( "invalid patch-cap type" );
		return;
	}
	CapTexture();
	controlPointsChanged();
}
#if 0
void Patch::ProjectTexture( int nAxis ){
	undoSave();

	int s, t;

	switch ( nAxis )
	{
	case 2:
		s = 0;
		t = 1;
		break;
	case 0:
		s = 1;
		t = 2;
		break;
	case 1:
		s = 0;
		t = 2;
		break;
	default:
		ERROR_MESSAGE( "invalid axis" );
		return;
	}

	float fWidth = 1 / ( m_state->getTexture().width * Texdef_getDefaultTextureScale() );
	float fHeight = 1 / ( m_state->getTexture().height * -Texdef_getDefaultTextureScale() );

	for ( PatchControlIter i = m_ctrl.data(); i != m_ctrl.data() + m_ctrl.size(); ++i )
	{
		( *i ).m_texcoord[0] = ( *i ).m_vertex[s] * fWidth;
		( *i ).m_texcoord[1] = ( *i ).m_vertex[t] * fHeight;
	}

	controlPointsChanged();
}
#else
void Patch::ProjectTexture( TextureProjection projection, const Vector3& normal ){
	undoSave();

	projection.m_brushprimit_texdef.addScale( m_state->getTexture().width, m_state->getTexture().height );

	Matrix4 local2tex;
	Texdef_Construct_local2tex( projection, m_state->getTexture().width, m_state->getTexture().height, normal, local2tex );

	for ( PatchControlIter i = m_ctrl.data(); i != m_ctrl.data() + m_ctrl.size(); ++i )
	{
		( *i ).m_texcoord = matrix4_transformed_point( local2tex, ( *i ).m_vertex ).vec2();
	}

	controlPointsChanged();
	Patch_textureChanged();
}
#endif

void Patch::ProjectTexture( const texdef_t& texdef, const Vector3* direction ){
	undoSave();

	Matrix4 local2tex;
	Texdef_Construct_local2tex4projection( texdef, m_state->getTexture().width, m_state->getTexture().height, Calculate_AvgNormal(), direction, local2tex );

	for ( PatchControlIter i = m_ctrl.data(); i != m_ctrl.data() + m_ctrl.size(); ++i )
	{
		( *i ).m_texcoord = matrix4_transformed_point( local2tex, ( *i ).m_vertex ).vec2();
	}

	controlPointsChanged();
	Patch_textureChanged();
}

void Patch::constructPlane( const AABB& aabb, int axis, std::size_t width, std::size_t height ){
	setDims( width, height );

	int x, y, z;
	switch ( axis )
	{
	case 2: x = 0; y = 1; z = 2; break;
	case 1: x = 0; y = 2; z = 1; break;
	case 0: x = 1; y = 2; z = 0; break;
	default:
		ERROR_MESSAGE( "invalid view-type" );
		return;
	}

	if ( m_width < MIN_PATCH_WIDTH || m_width > MAX_PATCH_WIDTH ) {
		m_width = 3;
	}
	if ( m_height < MIN_PATCH_HEIGHT || m_height > MAX_PATCH_HEIGHT ) {
		m_height = 3;
	}

	Vector3 vStart;
	vStart[x] = aabb.origin[x] - aabb.extents[x];
	vStart[y] = aabb.origin[y] - aabb.extents[y];
	vStart[z] = aabb.origin[z] + aabb.extents[z];

	float xAdj = fabsf( ( vStart[x] - ( aabb.origin[x] + aabb.extents[x] ) ) / (float)( m_width - 1 ) );
	float yAdj = fabsf( ( vStart[y] - ( aabb.origin[y] + aabb.extents[y] ) ) / (float)( m_height - 1 ) );

	Vector3 vTmp;
	vTmp[z] = vStart[z];
	PatchControl* pCtrl = m_ctrl.data();

	vTmp[y] = vStart[y];
	for ( std::size_t h = 0; h < m_height; h++ )
	{
		vTmp[x] = vStart[x];
		for ( std::size_t w = 0; w < m_width; w++, ++pCtrl )
		{
			pCtrl->m_vertex = vTmp;
			vTmp[x] += xAdj;
		}
		vTmp[y] += yAdj;
	}
}

void Patch::ConstructPrefab( const AABB& aabb, EPatchPrefab eType, int axis, std::size_t width, std::size_t height ){
	Vector3 vPos[3];

	int x, y, z;
	switch ( axis )
	{
	case 2: x = 0; y = 1; z = 2; break;
	//case 1: x = 0; y = 2; z = 1; break;
	case 1: x = 2; y = 0; z = 1; break;
	case 0: x = 1; y = 2; z = 0; break;
	default:
		ERROR_MESSAGE( "invalid view-type" );
		return;
	}

	if ( eType != EPatchPrefab::Plane ) {
		vPos[0] = vector3_subtracted( aabb.origin, aabb.extents );
		vPos[1] = aabb.origin;
		vPos[2] = vector3_added( aabb.origin, aabb.extents );
	}

	if ( eType == EPatchPrefab::Plane ) {
		constructPlane( aabb, axis, width, height );
	}
	else if ( eType == EPatchPrefab::SqCylinder
	       || eType == EPatchPrefab::Cylinder
	       || eType == EPatchPrefab::DenseCylinder
	       || eType == EPatchPrefab::VeryDenseCylinder
	       || eType == EPatchPrefab::Cone
	       || eType == EPatchPrefab::Sphere ) {
		unsigned char *pIndex;
		unsigned char pCylIndex[] =
		{
			0, 0,
			1, 0,
			2, 0,
			2, 1,
			2, 2,
			1, 2,
			0, 2,
			0, 1,
			0, 0
		};


		PatchControl *pStart;
		switch ( eType )
		{
		case EPatchPrefab::SqCylinder:
			setDims( 9, 3 );
			pStart = m_ctrl.data();
			break;
		case EPatchPrefab::DenseCylinder:
		case EPatchPrefab::VeryDenseCylinder:
		case EPatchPrefab::Cylinder:
			setDims( 9, 3 );
			pStart = m_ctrl.data() + 1;
			break;
		case EPatchPrefab::Cone:
			setDims( 9, 3 );
			pStart = m_ctrl.data() + 1;
			break;
		case EPatchPrefab::Sphere:
			setDims( 9, 5 );
			pStart = m_ctrl.data() + ( 9 + 1 );
			break;
		default:
			ERROR_MESSAGE( "this should be unreachable" );
			return;
		}

		for ( std::size_t h = 0; h < 3; h++, pStart += 9 )
		{
			pIndex = pCylIndex;
			PatchControl* pCtrl = pStart;
			for ( std::size_t w = 0; w < 8; w++, pCtrl++ )
			{
				pCtrl->m_vertex[x] = vPos[pIndex[0]][x];
				pCtrl->m_vertex[y] = vPos[pIndex[1]][y];
				pCtrl->m_vertex[z] = vPos[h][z];
				pIndex += 2;
			}
		}

		switch ( eType )
		{
		case EPatchPrefab::SqCylinder:
			{
				PatchControl* pCtrl = m_ctrl.data();
				for ( std::size_t h = 0; h < 3; h++, pCtrl += 9 )
				{
					pCtrl[8].m_vertex = pCtrl[0].m_vertex;
				}
			}
			break;
		case EPatchPrefab::DenseCylinder:
		case EPatchPrefab::VeryDenseCylinder:
		case EPatchPrefab::Cylinder:
			{
				PatchControl* pCtrl = m_ctrl.data();
				for ( std::size_t h = 0; h < 3; h++, pCtrl += 9 )
				{
					pCtrl[0].m_vertex = pCtrl[8].m_vertex;
				}
			}
			break;
		case EPatchPrefab::Cone:
			{
				PatchControl* pCtrl = m_ctrl.data();
				for ( std::size_t h = 0; h < 2; h++, pCtrl += 9 )
				{
					pCtrl[0].m_vertex = pCtrl[8].m_vertex;
				}
			}
			{
				PatchControl* pCtrl = m_ctrl.data() + 9 * 2;
				for ( std::size_t w = 0; w < 9; w++, pCtrl++ )
				{
					pCtrl->m_vertex[x] = vPos[1][x];
					pCtrl->m_vertex[y] = vPos[1][y];
					pCtrl->m_vertex[z] = vPos[2][z];
				}
			}
			break;
		case EPatchPrefab::Sphere:
			{
				PatchControl* pCtrl = m_ctrl.data() + 9;
				for ( std::size_t h = 0; h < 3; h++, pCtrl += 9 )
				{
					pCtrl[0].m_vertex = pCtrl[8].m_vertex;
				}
			}
			{
				PatchControl* pCtrl = m_ctrl.data();
				for ( std::size_t w = 0; w < 9; w++, pCtrl++ )
				{
					pCtrl->m_vertex[x] = vPos[1][x];
					pCtrl->m_vertex[y] = vPos[1][y];
					pCtrl->m_vertex[z] = vPos[0][z];
				}
			}
			{
				PatchControl* pCtrl = m_ctrl.data() + ( 9 * 4 );
				for ( std::size_t w = 0; w < 9; w++, pCtrl++ )
				{
					pCtrl->m_vertex[x] = vPos[1][x];
					pCtrl->m_vertex[y] = vPos[1][y];
					pCtrl->m_vertex[z] = vPos[2][z];
				}
			}
			break;
		default:
			ERROR_MESSAGE( "this should be unreachable" );
			return;
		}
	}
	else if ( eType == EPatchPrefab::ExactCylinder ) {
		int n = ( width - 1 ) / 2; // n = number of segments
		setDims( width, height );

		// vPos[0] = vector3_subtracted( aabb.origin, aabb.extents );
		// vPos[1] = aabb.origin;
		// vPos[2] = vector3_added( aabb.origin, aabb.extents );

		unsigned int i, j;
		float f = 1 / cos( M_PI / n );
		for ( i = 0; i < width; ++i )
		{
			float angle = ( M_PI * i ) / n; // 0 to 2pi
			float x_ = vPos[1][x] + ( vPos[2][x] - vPos[1][x] ) * cos( angle ) * ( ( i & 1 ) ? f : 1.0f );
			float y_ = vPos[1][y] + ( vPos[2][y] - vPos[1][y] ) * sin( angle ) * ( ( i & 1 ) ? f : 1.0f );
			for ( j = 0; j < height; ++j )
			{
				float z_ = vPos[0][z] + ( vPos[2][z] - vPos[0][z] ) * ( j / (float)( height - 1 ) );
				PatchControl *v;
				v = &m_ctrl.data()[j * width + i];
				v->m_vertex[x] = x_;
				v->m_vertex[y] = y_;
				v->m_vertex[z] = z_;
			}
		}
	}
	else if ( eType == EPatchPrefab::ExactCone ) {
		int n = ( width - 1 ) / 2; // n = number of segments
		setDims( width, height );

		// vPos[0] = vector3_subtracted( aabb.origin, aabb.extents );
		// vPos[1] = aabb.origin;
		// vPos[2] = vector3_added( aabb.origin, aabb.extents );

		unsigned int i, j;
		float f = 1 / cos( M_PI / n );
		for ( i = 0; i < width; ++i )
		{
			float angle = ( M_PI * i ) / n;
			for ( j = 0; j < height; ++j )
			{
				float x_ = vPos[1][x] + ( 1.0f - ( j / (float)( height - 1 ) ) ) * ( vPos[2][x] - vPos[1][x] ) * cos( angle ) * ( ( i & 1 ) ? f : 1.0f );
				float y_ = vPos[1][y] + ( 1.0f - ( j / (float)( height - 1 ) ) ) * ( vPos[2][y] - vPos[1][y] ) * sin( angle ) * ( ( i & 1 ) ? f : 1.0f );
				float z_ = vPos[0][z] + ( vPos[2][z] - vPos[0][z] ) * ( j / (float)( height - 1 ) );
				PatchControl *v;
				v = &m_ctrl.data()[j * width + i];
				v->m_vertex[x] = x_;
				v->m_vertex[y] = y_;
				v->m_vertex[z] = z_;
			}
		}
	}
	else if ( eType == EPatchPrefab::ExactSphere ) {
		int n = ( width - 1 ) / 2; // n = number of segments (yaw)
		int m = ( height - 1 ) / 2; // m = number of segments (pitch)
		setDims( width, height );

		// vPos[0] = vector3_subtracted( aabb.origin, aabb.extents );
		// vPos[1] = aabb.origin;
		// vPos[2] = vector3_added( aabb.origin, aabb.extents );

		unsigned int i, j;
		float f = 1 / cos( M_PI / n );
		float g = 1 / cos( M_PI / ( 2 * m ) );
		for ( i = 0; i < width; ++i )
		{
			float angle = ( M_PI * i ) / n;
			for ( j = 0; j < height; ++j )
			{
				float angle2 = ( M_PI * j ) / ( 2 * m );
				float x_ = vPos[1][x] + ( vPos[2][x] - vPos[1][x] ) *  sin( angle2 ) * ( ( j & 1 ) ? g : 1.0f ) * cos( angle ) * ( ( i & 1 ) ? f : 1.0f );
				float y_ = vPos[1][y] + ( vPos[2][y] - vPos[1][y] ) *  sin( angle2 ) * ( ( j & 1 ) ? g : 1.0f ) * sin( angle ) * ( ( i & 1 ) ? f : 1.0f );
				float z_ = vPos[1][z] + ( vPos[2][z] - vPos[1][z] ) * -cos( angle2 ) * ( ( j & 1 ) ? g : 1.0f );
				PatchControl *v;
				v = &m_ctrl.data()[j * width + i];
				v->m_vertex[x] = x_;
				v->m_vertex[y] = y_;
				v->m_vertex[z] = z_;
			}
		}
	}
	else if  ( eType == EPatchPrefab::Bevel ) {
		unsigned char *pIndex;
		unsigned char pBevIndex[] =
		{
			0, 0,
			2, 0,
			2, 2,
		};

		setDims( 3, 3 );

		PatchControl* pCtrl = m_ctrl.data();
		for ( std::size_t h = 0; h < 3; h++ )
		{
			pIndex = pBevIndex;
			for ( std::size_t w = 0; w < 3; w++, pIndex += 2, pCtrl++ )
			{
				pCtrl->m_vertex[x] = vPos[pIndex[0]][x];
				pCtrl->m_vertex[y] = vPos[pIndex[1]][y];
				pCtrl->m_vertex[z] = vPos[h][z];
			}
		}
	}
	else if ( eType == EPatchPrefab::EndCap ) {
		unsigned char *pIndex;
		unsigned char pEndIndex[] =
		{
			2, 0,
			2, 2,
			1, 2,
			0, 2,
			0, 0,
		};

		setDims( 5, 3 );

		PatchControl* pCtrl = m_ctrl.data();
		for ( std::size_t h = 0; h < 3; h++ )
		{
			pIndex = pEndIndex;
			for ( std::size_t w = 0; w < 5; w++, pIndex += 2, pCtrl++ )
			{
				pCtrl->m_vertex[x] = vPos[pIndex[0]][x];
				pCtrl->m_vertex[y] = vPos[pIndex[1]][y];
				pCtrl->m_vertex[z] = vPos[h][z];
			}
		}
	}

	if ( eType == EPatchPrefab::DenseCylinder ) {
		InsertRemove( true, false, true );
	}

	if ( eType == EPatchPrefab::VeryDenseCylinder ) {
		InsertRemove( true, false, false );
		InsertRemove( true, false, true );
	}

	if ( eType == EPatchPrefab::Plane )
		CapTexture();
	else
		NaturalTexture();
}

void Patch::RenderDebug( RenderStateFlags state ) const {
	for ( std::size_t i = 0; i < m_tess.m_numStrips; i++ )
	{
		gl().glBegin( GL_QUAD_STRIP );
		for ( std::size_t j = 0; j < m_tess.m_lenStrips; j++ )
		{
			gl().glNormal3fv( normal3f_to_array( ( m_tess.m_vertices.data() + m_tess.m_indices[i * m_tess.m_lenStrips + j] )->normal ) );
			gl().glTexCoord2fv( texcoord2f_to_array( ( m_tess.m_vertices.data() + m_tess.m_indices[i * m_tess.m_lenStrips + j] )->texcoord ) );
			gl().glVertex3fv( vertex3f_to_array( ( m_tess.m_vertices.data() + m_tess.m_indices[i * m_tess.m_lenStrips + j] )->vertex ) );
		}
		gl().glEnd();
	}
}

void RenderablePatchSolid::RenderNormals() const {
	const std::size_t width = m_tess.m_numStrips + 1;
	const std::size_t height = m_tess.m_lenStrips >> 1;
	gl().glBegin( GL_LINES );
	for ( std::size_t i = 0; i < width; i++ )
	{
		for ( std::size_t j = 0; j < height; j++ )
		{
			{
				Vector3 vNormal(
				    vector3_added(
				        vertex3f_to_vector3( ( m_tess.m_vertices.data() + ( j * width + i ) )->vertex ),
				        vector3_scaled( normal3f_to_vector3( ( m_tess.m_vertices.data() + ( j * width + i ) )->normal ), 8 )
				    )
				);
				gl().glVertex3fv( vertex3f_to_array( ( m_tess.m_vertices.data() + ( j * width + i ) )->vertex ) );
				gl().glVertex3fv( &vNormal[0] );
			}
			{
				Vector3 vNormal(
				    vector3_added(
				        vertex3f_to_vector3( ( m_tess.m_vertices.data() + ( j * width + i ) )->vertex ),
				        vector3_scaled( normal3f_to_vector3( ( m_tess.m_vertices.data() + ( j * width + i ) )->tangent ), 8 )
				    )
				);
				gl().glVertex3fv( vertex3f_to_array( ( m_tess.m_vertices.data() + ( j * width + i ) )->vertex ) );
				gl().glVertex3fv( &vNormal[0] );
			}
			{
				Vector3 vNormal(
				    vector3_added(
				        vertex3f_to_vector3( ( m_tess.m_vertices.data() + ( j * width + i ) )->vertex ),
				        vector3_scaled( normal3f_to_vector3( ( m_tess.m_vertices.data() + ( j * width + i ) )->bitangent ), 8 )
				    )
				);
				gl().glVertex3fv( vertex3f_to_array( ( m_tess.m_vertices.data() + ( j * width + i ) )->vertex ) );
				gl().glVertex3fv( &vNormal[0] );
			}
		}
	}
	gl().glEnd();
}

#define DEGEN_0a  0x01
#define DEGEN_1a  0x02
#define DEGEN_2a  0x04
#define DEGEN_0b  0x08
#define DEGEN_1b  0x10
#define DEGEN_2b  0x20
#define SPLIT     0x40
#define AVERAGE   0x80


unsigned int subarray_get_degen( PatchControlIter subarray, std::size_t strideU, std::size_t strideV ){
	unsigned int nDegen = 0;
	const PatchControl* p1;
	const PatchControl* p2;

	p1 = subarray;
	p2 = p1 + strideU;
	if ( vector3_equal( p1->m_vertex, p2->m_vertex ) ) {
		nDegen |= DEGEN_0a;
	}
	p1 = p2;
	p2 = p1 + strideU;
	if ( vector3_equal( p1->m_vertex, p2->m_vertex ) ) {
		nDegen |= DEGEN_0b;
	}

	p1 = subarray + strideV;
	p2 = p1 + strideU;
	if ( vector3_equal( p1->m_vertex, p2->m_vertex ) ) {
		nDegen |= DEGEN_1a;
	}
	p1 = p2;
	p2 = p1 + strideU;
	if ( vector3_equal( p1->m_vertex, p2->m_vertex ) ) {
		nDegen |= DEGEN_1b;
	}

	p1 = subarray + ( strideV << 1 );
	p2 = p1 + strideU;
	if ( vector3_equal( p1->m_vertex, p2->m_vertex ) ) {
		nDegen |= DEGEN_2a;
	}
	p1 = p2;
	p2 = p1 + strideU;
	if ( vector3_equal( p1->m_vertex, p2->m_vertex ) ) {
		nDegen |= DEGEN_2b;
	}

	return nDegen;
}


inline void deCasteljau3( const Vector3& P0, const Vector3& P1, const Vector3& P2, Vector3& P01, Vector3& P12, Vector3& P012 ){
	P01 = vector3_mid( P0, P1 );
	P12 = vector3_mid( P1, P2 );
	P012 = vector3_mid( P01, P12 );
}

inline void BezierInterpolate3( const Vector3& start, Vector3& left, Vector3& mid, Vector3& right, const Vector3& end ){
	left = vector3_mid( start, mid );
	right = vector3_mid( mid, end );
	mid = vector3_mid( left, right );
}

inline void BezierInterpolate2( const Vector2& start, Vector2& left, Vector2& mid, Vector2& right, const Vector2& end ){
	left[0] = float_mid( start[0], mid[0] );
	left[1] = float_mid( start[1], mid[1] );
	right[0] = float_mid( mid[0], end[0] );
	right[1] = float_mid( mid[1], end[1] );
	mid[0] = float_mid( left[0], right[0] );
	mid[1] = float_mid( left[1], right[1] );
}


inline Vector2& texcoord_for_index( Array<ArbitraryMeshVertex>& vertices, std::size_t index ){
	return reinterpret_cast<Vector2&>( vertices[index].texcoord );
}

inline Vector3& vertex_for_index( Array<ArbitraryMeshVertex>& vertices, std::size_t index ){
	return reinterpret_cast<Vector3&>( vertices[index].vertex );
}

inline Vector3& normal_for_index( Array<ArbitraryMeshVertex>& vertices, std::size_t index ){
	return reinterpret_cast<Vector3&>( vertices[index].normal );
}

inline Vector3& tangent_for_index( Array<ArbitraryMeshVertex>& vertices, std::size_t index ){
	return reinterpret_cast<Vector3&>( vertices[index].tangent );
}

inline Vector3& bitangent_for_index( Array<ArbitraryMeshVertex>& vertices, std::size_t index ){
	return reinterpret_cast<Vector3&>( vertices[index].bitangent );
}

inline const Vector2& texcoord_for_index( const Array<ArbitraryMeshVertex>& vertices, std::size_t index ){
	return reinterpret_cast<const Vector2&>( vertices[index].texcoord );
}

inline const Vector3& vertex_for_index( const Array<ArbitraryMeshVertex>& vertices, std::size_t index ){
	return reinterpret_cast<const Vector3&>( vertices[index].vertex );
}

inline const Vector3& normal_for_index( const Array<ArbitraryMeshVertex>& vertices, std::size_t index ){
	return reinterpret_cast<const Vector3&>( vertices[index].normal );
}

inline const Vector3& tangent_for_index( const Array<ArbitraryMeshVertex>& vertices, std::size_t index ){
	return reinterpret_cast<const Vector3&>( vertices[index].tangent );
}

inline const Vector3& bitangent_for_index( const Array<ArbitraryMeshVertex>& vertices, std::size_t index ){
	return reinterpret_cast<const Vector3&>( vertices[index].bitangent );
}

#include "math/curve.h"

inline PatchControl QuadraticBezier_evaluate( const PatchControl* firstPoint, double t ){
	PatchControl result = { Vector3( 0, 0, 0 ), Vector2( 0, 0 ) };
	double denominator = 0;

	{
		double weight = BernsteinPolynomial<0, 2>( t );
		vector3_add( result.m_vertex, vector3_scaled( firstPoint[0].m_vertex, weight ) );
		vector2_add( result.m_texcoord, vector2_scaled( firstPoint[0].m_texcoord, weight ) );
		denominator += weight;
	}
	{
		double weight = BernsteinPolynomial<1, 2>( t );
		vector3_add( result.m_vertex, vector3_scaled( firstPoint[1].m_vertex, weight ) );
		vector2_add( result.m_texcoord, vector2_scaled( firstPoint[1].m_texcoord, weight ) );
		denominator += weight;
	}
	{
		double weight = BernsteinPolynomial<2, 2>( t );
		vector3_add( result.m_vertex, vector3_scaled( firstPoint[2].m_vertex, weight ) );
		vector2_add( result.m_texcoord, vector2_scaled( firstPoint[2].m_texcoord, weight ) );
		denominator += weight;
	}

	vector3_divide( result.m_vertex, denominator );
	vector2_divide( result.m_texcoord, denominator );
	return result;
}

inline Vector3 vector3_linear_interpolated( const Vector3& a, const Vector3& b, double t ){
	return vector3_added( vector3_scaled( a, 1.0 - t ), vector3_scaled( b, t ) );
}

inline Vector2 vector2_linear_interpolated( const Vector2& a, const Vector2& b, double t ){
	return vector2_added( vector2_scaled( a, 1.0 - t ), vector2_scaled( b, t ) );
}

void normalise_safe( Vector3& normal ){
	if ( !vector3_equal( normal, g_vector3_identity ) ) {
		vector3_normalise( normal );
	}
}

inline void QuadraticBezier_evaluate( const PatchControl& a, const PatchControl& b, const PatchControl& c, double t, PatchControl& point, PatchControl& left, PatchControl& right ){
	left.m_vertex = vector3_linear_interpolated( a.m_vertex, b.m_vertex, t );
	left.m_texcoord = vector2_linear_interpolated( a.m_texcoord, b.m_texcoord, t );
	right.m_vertex = vector3_linear_interpolated( b.m_vertex, c.m_vertex, t );
	right.m_texcoord = vector2_linear_interpolated( b.m_texcoord, c.m_texcoord, t );
	point.m_vertex = vector3_linear_interpolated( left.m_vertex, right.m_vertex, t );
	point.m_texcoord = vector2_linear_interpolated( left.m_texcoord, right.m_texcoord, t );
}

void Patch::TesselateSubMatrixFixed( ArbitraryMeshVertex* vertices, std::size_t strideX, std::size_t strideY, unsigned int nFlagsX, unsigned int nFlagsY, PatchControl* subMatrix[3][3] ){
	double incrementU = 1.0 / m_subdivisions_x;
	double incrementV = 1.0 / m_subdivisions_y;
	const std::size_t width = m_subdivisions_x + 1;
	const std::size_t height = m_subdivisions_y + 1;

	for ( std::size_t i = 0; i != width; ++i )
	{
		double tU = ( i + 1 == width ) ? 1 : i * incrementU;
		PatchControl pointX[3];
		PatchControl leftX[3];
		PatchControl rightX[3];
		QuadraticBezier_evaluate( *subMatrix[0][0], *subMatrix[0][1], *subMatrix[0][2], tU, pointX[0], leftX[0], rightX[0] );
		QuadraticBezier_evaluate( *subMatrix[1][0], *subMatrix[1][1], *subMatrix[1][2], tU, pointX[1], leftX[1], rightX[1] );
		QuadraticBezier_evaluate( *subMatrix[2][0], *subMatrix[2][1], *subMatrix[2][2], tU, pointX[2], leftX[2], rightX[2] );

		ArbitraryMeshVertex* p = vertices + i * strideX;
		for ( std::size_t j = 0; j != height; ++j )
		{
			if ( ( j == 0 || j + 1 == height ) && ( i == 0 || i + 1 == width ) ) {
			}
			else
			{
				double tV = ( j + 1 == height ) ? 1 : j * incrementV;

				PatchControl pointY[3];
				PatchControl leftY[3];
				PatchControl rightY[3];
				QuadraticBezier_evaluate( *subMatrix[0][0], *subMatrix[1][0], *subMatrix[2][0], tV, pointY[0], leftY[0], rightY[0] );
				QuadraticBezier_evaluate( *subMatrix[0][1], *subMatrix[1][1], *subMatrix[2][1], tV, pointY[1], leftY[1], rightY[1] );
				QuadraticBezier_evaluate( *subMatrix[0][2], *subMatrix[1][2], *subMatrix[2][2], tV, pointY[2], leftY[2], rightY[2] );

				PatchControl point;
				PatchControl left;
				PatchControl right;
				QuadraticBezier_evaluate( pointX[0], pointX[1], pointX[2], tV, point, left, right );
				PatchControl up;
				PatchControl down;
				QuadraticBezier_evaluate( pointY[0], pointY[1], pointY[2], tU, point, up, down );

				vertex3f_to_vector3( p->vertex ) = point.m_vertex;
				texcoord2f_to_vector2( p->texcoord ) = point.m_texcoord;

				ArbitraryMeshVertex a, b, c;

				a.vertex = vertex3f_for_vector3( left.m_vertex );
				a.texcoord = texcoord2f_for_vector2( left.m_texcoord );
				b.vertex = vertex3f_for_vector3( right.m_vertex );
				b.texcoord = texcoord2f_for_vector2( right.m_texcoord );

				if ( i != 0 ) {
					c.vertex = vertex3f_for_vector3( up.m_vertex );
					c.texcoord = texcoord2f_for_vector2( up.m_texcoord );
				}
				else
				{
					c.vertex = vertex3f_for_vector3( down.m_vertex );
					c.texcoord = texcoord2f_for_vector2( down.m_texcoord );
				}

				Vector3 normal = vector3_normalised( vector3_cross( right.m_vertex - left.m_vertex, up.m_vertex - down.m_vertex ) );

				Vector3 tangent, bitangent;
				ArbitraryMeshTriangle_calcTangents( a, b, c, tangent, bitangent );
				vector3_normalise( tangent );
				vector3_normalise( bitangent );

				if ( ( ( nFlagsX & AVERAGE ) != 0 && i == 0 ) || ( ( nFlagsY & AVERAGE ) != 0  && j == 0 ) ) {
					normal3f_to_vector3( p->normal ) = vector3_normalised( vector3_added( normal3f_to_vector3( p->normal ), normal ) );
					normal3f_to_vector3( p->tangent ) = vector3_normalised( vector3_added( normal3f_to_vector3( p->tangent ), tangent ) );
					normal3f_to_vector3( p->bitangent ) = vector3_normalised( vector3_added( normal3f_to_vector3( p->bitangent ), bitangent ) );
				}
				else
				{
					normal3f_to_vector3( p->normal ) = normal;
					normal3f_to_vector3( p->tangent ) = tangent;
					normal3f_to_vector3( p->bitangent ) = bitangent;
				}
			}

			p += strideY;
		}
	}
}

void Patch::TesselateSubMatrix( const BezierCurveTree *BX, const BezierCurveTree *BY,
                                std::size_t offStartX, std::size_t offStartY,
                                std::size_t offEndX, std::size_t offEndY,
                                std::size_t nFlagsX, std::size_t nFlagsY,
                                Vector3& left, Vector3& mid, Vector3& right,
                                Vector2& texLeft, Vector2& texMid, Vector2& texRight,
                                bool bTranspose ){
	int newFlagsX, newFlagsY;

	Vector3 tmp;
	Vector3 vertex_0_0, vertex_0_1, vertex_1_0, vertex_1_1, vertex_2_0, vertex_2_1;
	Vector2 texTmp;
	Vector2 texcoord_0_0, texcoord_0_1, texcoord_1_0, texcoord_1_1, texcoord_2_0, texcoord_2_1;

	{
		// texcoords

		BezierInterpolate2( texcoord_for_index( m_tess.m_vertices, offStartX + offStartY ),
		                    texcoord_0_0,
		                    texcoord_for_index( m_tess.m_vertices, BX->index + offStartY ),
		                    texcoord_0_1,
		                    texcoord_for_index( m_tess.m_vertices, offEndX + offStartY ) );


		BezierInterpolate2( texcoord_for_index( m_tess.m_vertices, offStartX + offEndY ),
		                    texcoord_2_0,
		                    texcoord_for_index( m_tess.m_vertices, BX->index + offEndY ),
		                    texcoord_2_1,
		                    texcoord_for_index( m_tess.m_vertices, offEndX + offEndY ) );

		texTmp = texMid;

		BezierInterpolate2( texLeft,
		                    texcoord_1_0,
		                    texTmp,
		                    texcoord_1_1,
		                    texRight );

		if ( !BezierCurveTree_isLeaf( BY ) ) {
			texcoord_for_index( m_tess.m_vertices, BX->index + BY->index ) = texTmp;
		}


		if ( !BezierCurveTree_isLeaf( BX->left ) ) {
			texcoord_for_index( m_tess.m_vertices, BX->left->index + offStartY ) = texcoord_0_0;
			texcoord_for_index( m_tess.m_vertices, BX->left->index + offEndY ) = texcoord_2_0;

			if ( !BezierCurveTree_isLeaf( BY ) ) {
				texcoord_for_index( m_tess.m_vertices, BX->left->index + BY->index ) = texcoord_1_0;
			}
		}
		if ( !BezierCurveTree_isLeaf( BX->right ) ) {
			texcoord_for_index( m_tess.m_vertices, BX->right->index + offStartY ) = texcoord_0_1;
			texcoord_for_index( m_tess.m_vertices, BX->right->index + offEndY ) = texcoord_2_1;

			if ( !BezierCurveTree_isLeaf( BY ) ) {
				texcoord_for_index( m_tess.m_vertices, BX->right->index + BY->index ) = texcoord_1_1;
			}
		}


		// verts

		BezierInterpolate3( vertex_for_index( m_tess.m_vertices, offStartX + offStartY ),
		                    vertex_0_0,
		                    vertex_for_index( m_tess.m_vertices, BX->index + offStartY ),
		                    vertex_0_1,
		                    vertex_for_index( m_tess.m_vertices, offEndX + offStartY ) );


		BezierInterpolate3( vertex_for_index( m_tess.m_vertices, offStartX + offEndY ),
		                    vertex_2_0,
		                    vertex_for_index( m_tess.m_vertices, BX->index + offEndY ),
		                    vertex_2_1,
		                    vertex_for_index( m_tess.m_vertices, offEndX + offEndY ) );


		tmp = mid;

		BezierInterpolate3( left,
		                    vertex_1_0,
		                    tmp,
		                    vertex_1_1,
		                    right );

		if ( !BezierCurveTree_isLeaf( BY ) ) {
			vertex_for_index( m_tess.m_vertices, BX->index + BY->index ) = tmp;
		}


		if ( !BezierCurveTree_isLeaf( BX->left ) ) {
			vertex_for_index( m_tess.m_vertices, BX->left->index + offStartY ) = vertex_0_0;
			vertex_for_index( m_tess.m_vertices, BX->left->index + offEndY ) = vertex_2_0;

			if ( !BezierCurveTree_isLeaf( BY ) ) {
				vertex_for_index( m_tess.m_vertices, BX->left->index + BY->index ) = vertex_1_0;
			}
		}
		if ( !BezierCurveTree_isLeaf( BX->right ) ) {
			vertex_for_index( m_tess.m_vertices, BX->right->index + offStartY ) = vertex_0_1;
			vertex_for_index( m_tess.m_vertices, BX->right->index + offEndY ) = vertex_2_1;

			if ( !BezierCurveTree_isLeaf( BY ) ) {
				vertex_for_index( m_tess.m_vertices, BX->right->index + BY->index ) = vertex_1_1;
			}
		}

		// normals

		if ( nFlagsX & SPLIT ) {
			ArbitraryMeshVertex a, b, c;
			Vector3 tangentU;

			if ( !( nFlagsX & DEGEN_0a ) || !( nFlagsX & DEGEN_0b ) ) {
				tangentU = vector3_subtracted( vertex_0_1, vertex_0_0 );
				a.vertex = vertex3f_for_vector3( vertex_0_0 );
				a.texcoord = texcoord2f_for_vector2( texcoord_0_0 );
				c.vertex = vertex3f_for_vector3( vertex_0_1 );
				c.texcoord = texcoord2f_for_vector2( texcoord_0_1 );
			}
			else if ( !( nFlagsX & DEGEN_1a ) || !( nFlagsX & DEGEN_1b ) ) {
				tangentU = vector3_subtracted( vertex_1_1, vertex_1_0 );
				a.vertex = vertex3f_for_vector3( vertex_1_0 );
				a.texcoord = texcoord2f_for_vector2( texcoord_1_0 );
				c.vertex = vertex3f_for_vector3( vertex_1_1 );
				c.texcoord = texcoord2f_for_vector2( texcoord_1_1 );
			}
			else
			{
				tangentU = vector3_subtracted( vertex_2_1, vertex_2_0 );
				a.vertex = vertex3f_for_vector3( vertex_2_0 );
				a.texcoord = texcoord2f_for_vector2( texcoord_2_0 );
				c.vertex = vertex3f_for_vector3( vertex_2_1 );
				c.texcoord = texcoord2f_for_vector2( texcoord_2_1 );
			}

			Vector3 tangentV;

			if ( ( nFlagsY & DEGEN_0a ) && ( nFlagsY & DEGEN_1a ) && ( nFlagsY & DEGEN_2a ) ) {
				tangentV = vector3_subtracted( vertex_for_index( m_tess.m_vertices, BX->index + offEndY ), tmp );
				b.vertex = vertex3f_for_vector3( tmp ); //m_tess.m_vertices[BX->index + offEndY].vertex;
				b.texcoord = texcoord2f_for_vector2( texTmp ); //m_tess.m_vertices[BX->index + offEndY].texcoord;
			}
			else
			{
				tangentV = vector3_subtracted( tmp, vertex_for_index( m_tess.m_vertices, BX->index + offStartY ) );
				b.vertex = vertex3f_for_vector3( tmp ); //m_tess.m_vertices[BX->index + offStartY].vertex;
				b.texcoord = texcoord2f_for_vector2( texTmp ); //m_tess.m_vertices[BX->index + offStartY].texcoord;
			}


			Vector3 normal, s, t;
			ArbitraryMeshVertex& v = m_tess.m_vertices[offStartY + BX->index];
			Vector3& p = normal3f_to_vector3( v.normal );
			Vector3& ps = normal3f_to_vector3( v.tangent );
			Vector3& pt = normal3f_to_vector3( v.bitangent );

			if ( bTranspose ) {
				normal = vector3_cross( tangentV, tangentU );
			}
			else
			{
				normal = vector3_cross( tangentU, tangentV );
			}
			normalise_safe( normal );

			ArbitraryMeshTriangle_calcTangents( a, b, c, s, t );
			normalise_safe( s );
			normalise_safe( t );

			if ( nFlagsX & AVERAGE ) {
				p = vector3_normalised( vector3_added( p, normal ) );
				ps = vector3_normalised( vector3_added( ps, s ) );
				pt = vector3_normalised( vector3_added( pt, t ) );
			}
			else
			{
				p = normal;
				ps = s;
				pt = t;
			}
		}

		{
			ArbitraryMeshVertex a, b, c;
			Vector3 tangentU;

			if ( !( nFlagsX & DEGEN_2a ) || !( nFlagsX & DEGEN_2b ) ) {
				tangentU = vector3_subtracted( vertex_2_1, vertex_2_0 );
				a.vertex = vertex3f_for_vector3( vertex_2_0 );
				a.texcoord = texcoord2f_for_vector2( texcoord_2_0 );
				c.vertex = vertex3f_for_vector3( vertex_2_1 );
				c.texcoord = texcoord2f_for_vector2( texcoord_2_1 );
			}
			else if ( !( nFlagsX & DEGEN_1a ) || !( nFlagsX & DEGEN_1b ) ) {
				tangentU = vector3_subtracted( vertex_1_1, vertex_1_0 );
				a.vertex = vertex3f_for_vector3( vertex_1_0 );
				a.texcoord = texcoord2f_for_vector2( texcoord_1_0 );
				c.vertex = vertex3f_for_vector3( vertex_1_1 );
				c.texcoord = texcoord2f_for_vector2( texcoord_1_1 );
			}
			else
			{
				tangentU = vector3_subtracted( vertex_0_1, vertex_0_0 );
				a.vertex = vertex3f_for_vector3( vertex_0_0 );
				a.texcoord = texcoord2f_for_vector2( texcoord_0_0 );
				c.vertex = vertex3f_for_vector3( vertex_0_1 );
				c.texcoord = texcoord2f_for_vector2( texcoord_0_1 );
			}

			Vector3 tangentV;

			if ( ( nFlagsY & DEGEN_0b ) && ( nFlagsY & DEGEN_1b ) && ( nFlagsY & DEGEN_2b ) ) {
				tangentV = vector3_subtracted( tmp, vertex_for_index( m_tess.m_vertices, BX->index + offStartY ) );
				b.vertex = vertex3f_for_vector3( tmp ); //m_tess.m_vertices[BX->index + offStartY].vertex;
				b.texcoord = texcoord2f_for_vector2( texTmp ); //m_tess.m_vertices[BX->index + offStartY].texcoord;
			}
			else
			{
				tangentV = vector3_subtracted( vertex_for_index( m_tess.m_vertices, BX->index + offEndY ), tmp );
				b.vertex = vertex3f_for_vector3( tmp ); //m_tess.m_vertices[BX->index + offEndY].vertex;
				b.texcoord = texcoord2f_for_vector2( texTmp ); //m_tess.m_vertices[BX->index + offEndY].texcoord;
			}

			ArbitraryMeshVertex& v = m_tess.m_vertices[offEndY + BX->index];
			Vector3& p = normal3f_to_vector3( v.normal );
			Vector3& ps = normal3f_to_vector3( v.tangent );
			Vector3& pt = normal3f_to_vector3( v.bitangent );

			if ( bTranspose ) {
				p = vector3_cross( tangentV, tangentU );
			}
			else
			{
				p = vector3_cross( tangentU, tangentV );
			}
			normalise_safe( p );

			ArbitraryMeshTriangle_calcTangents( a, b, c, ps, pt );
			normalise_safe( ps );
			normalise_safe( pt );
		}
	}


	newFlagsX = newFlagsY = 0;

	if ( ( nFlagsX & DEGEN_0a ) && ( nFlagsX & DEGEN_0b ) ) {
		newFlagsX |= DEGEN_0a;
		newFlagsX |= DEGEN_0b;
	}
	if ( ( nFlagsX & DEGEN_1a ) && ( nFlagsX & DEGEN_1b ) ) {
		newFlagsX |= DEGEN_1a;
		newFlagsX |= DEGEN_1b;
	}
	if ( ( nFlagsX & DEGEN_2a ) && ( nFlagsX & DEGEN_2b ) ) {
		newFlagsX |= DEGEN_2a;
		newFlagsX |= DEGEN_2b;
	}
	if ( ( nFlagsY & DEGEN_0a ) && ( nFlagsY & DEGEN_1a ) && ( nFlagsY & DEGEN_2a ) ) {
		newFlagsY |= DEGEN_0a;
		newFlagsY |= DEGEN_1a;
		newFlagsY |= DEGEN_2a;
	}
	if ( ( nFlagsY & DEGEN_0b ) && ( nFlagsY & DEGEN_1b ) && ( nFlagsY & DEGEN_2b ) ) {
		newFlagsY |= DEGEN_0b;
		newFlagsY |= DEGEN_1b;
		newFlagsY |= DEGEN_2b;
	}


	//if( ( nFlagsX & DEGEN_0a ) && ( nFlagsX & DEGEN_1a ) && ( nFlagsX & DEGEN_2a ) ) { newFlagsX |= DEGEN_0a; newFlagsX |= DEGEN_1a; newFlagsX |= DEGEN_2a; }
	//if( ( nFlagsX & DEGEN_0b ) && ( nFlagsX & DEGEN_1b ) && ( nFlagsX & DEGEN_2b ) ) { newFlagsX |= DEGEN_0b; newFlagsX |= DEGEN_1b; newFlagsX |= DEGEN_2b; }

	newFlagsX |= ( nFlagsX & SPLIT );
	newFlagsX |= ( nFlagsX & AVERAGE );

	if ( !BezierCurveTree_isLeaf( BY ) ) {
		{
			int nTemp = newFlagsY;

			if ( ( nFlagsY & DEGEN_0a ) && ( nFlagsY & DEGEN_0b ) ) {
				newFlagsY |= DEGEN_0a;
				newFlagsY |= DEGEN_0b;
			}
			newFlagsY |= ( nFlagsY & SPLIT );
			newFlagsY |= ( nFlagsY & AVERAGE );

			Vector3& p = vertex_for_index( m_tess.m_vertices, BX->index + BY->index );
			Vector3 vTemp( p );

			Vector2& p2 = texcoord_for_index( m_tess.m_vertices, BX->index + BY->index );
			Vector2 stTemp( p2 );

			TesselateSubMatrix( BY, BX->left,
			                    offStartY, offStartX,
			                    offEndY, BX->index,
			                    newFlagsY, newFlagsX,
			                    vertex_0_0, vertex_1_0, vertex_2_0,
			                    texcoord_0_0, texcoord_1_0, texcoord_2_0,
			                    !bTranspose );

			newFlagsY = nTemp;
			p = vTemp;
			p2 = stTemp;
		}

		if ( ( nFlagsY & DEGEN_2a ) && ( nFlagsY & DEGEN_2b ) ) {
			newFlagsY |= DEGEN_2a;
			newFlagsY |= DEGEN_2b;
		}

		TesselateSubMatrix( BY, BX->right,
		                    offStartY, BX->index,
		                    offEndY, offEndX,
		                    newFlagsY, newFlagsX,
		                    vertex_0_1, vertex_1_1, vertex_2_1,
		                    texcoord_0_1, texcoord_1_1, texcoord_2_1,
		                    !bTranspose );
	}
	else
	{
		if ( !BezierCurveTree_isLeaf( BX->left ) ) {
			TesselateSubMatrix( BX->left,  BY,
			                    offStartX, offStartY,
			                    BX->index, offEndY,
			                    newFlagsX, newFlagsY,
			                    left, vertex_1_0, tmp,
			                    texLeft, texcoord_1_0, texTmp,
			                    bTranspose );
		}

		if ( !BezierCurveTree_isLeaf( BX->right ) ) {
			TesselateSubMatrix( BX->right, BY,
			                    BX->index, offStartY,
			                    offEndX, offEndY,
			                    newFlagsX, newFlagsY,
			                    tmp, vertex_1_1, right,
			                    texTmp, texcoord_1_1, texRight,
			                    bTranspose );
		}
	}

}

void Patch::BuildTesselationCurves( EMatrixMajor major ){
	std::size_t nArrayStride, length, cross, strideU, strideV;
	switch ( major )
	{
	case ROW:
		nArrayStride = 1;
		length = ( m_width - 1 ) >> 1;
		cross = m_height;
		strideU = 1;
		strideV = m_width;

		if ( !m_patchDef3 ) {
			BezierCurveTreeArray_deleteAll( m_tess.m_curveTreeU );
		}

		break;
	case COL:
		nArrayStride = m_tess.m_nArrayWidth;
		length = ( m_height - 1 ) >> 1;
		cross = m_width;
		strideU = m_width;
		strideV = 1;

		if ( !m_patchDef3 ) {
			BezierCurveTreeArray_deleteAll( m_tess.m_curveTreeV );
		}

		break;
	default:
		ERROR_MESSAGE( "neither row-major nor column-major" );
		return;
	}

	Array<std::size_t> arrayLength( length );
	Array<BezierCurveTree*> pCurveTree( length );

	std::size_t nArrayLength = 1;

	if ( m_patchDef3 ) {
		for ( Array<std::size_t>::iterator i = arrayLength.begin(); i != arrayLength.end(); ++i )
		{
			*i = Array<std::size_t>::value_type( ( major == ROW ) ? m_subdivisions_x : m_subdivisions_y );
			nArrayLength += *i;
		}
	}
	else
	{
		// create a list of the horizontal control curves in each column of sub-patches
		// adaptively tesselate each horizontal control curve in the list
		// create a binary tree representing the combined tesselation of the list
		for ( std::size_t i = 0; i != length; ++i )
		{
			PatchControl* p1 = m_ctrlTransformed.data() + ( i * 2 * strideU );
			std::forward_list<BezierCurve> curveList;
			for ( std::size_t j = 0; j < cross; j += 2 )
			{
				PatchControl* p2 = p1 + strideV;
				PatchControl* p3 = p2 + strideV;

				// directly taken from one row of control points
				{
					BezierCurve& curve = curveList.emplace_front();
					curve.crd = ( p1 + strideU )->m_vertex;
					curve.left = p1->m_vertex;
					curve.right = ( p1 + ( strideU << 1 ) )->m_vertex;
				}

				if ( j + 2 >= cross ) {
					break;
				}

				// interpolated from three columns of control points
				{
					BezierCurve& curve = curveList.emplace_front();
					curve.crd = vector3_mid( ( p1 + strideU )->m_vertex, ( p3 + strideU )->m_vertex );
					curve.left = vector3_mid( p1->m_vertex, p3->m_vertex );
					curve.right = vector3_mid( ( p1 + ( strideU << 1 ) )->m_vertex, ( p3 + ( strideU << 1 ) )->m_vertex );

					curve.crd = vector3_mid( curve.crd, ( p2 + strideU )->m_vertex );
					curve.left = vector3_mid( curve.left, p2->m_vertex );
					curve.right = vector3_mid( curve.right, ( p2 + ( strideU << 1 ) )->m_vertex );
				}

				p1 = p3;
			}

			pCurveTree[i] = new BezierCurveTree;
			BezierCurveTree_FromCurveList( pCurveTree[i], curveList );

			// set up array indices for binary tree
			// accumulate subarray width
			arrayLength[i] = Array<std::size_t>::value_type( BezierCurveTree_Setup( pCurveTree[i], nArrayLength, nArrayStride ) - ( nArrayLength - 1 ) );
			// accumulate total array width
			nArrayLength += arrayLength[i];
		}
	}

	switch ( major )
	{
	case ROW:
		m_tess.m_nArrayWidth = nArrayLength;
		std::swap( m_tess.m_arrayWidth, arrayLength );

		if ( !m_patchDef3 ) {
			std::swap( m_tess.m_curveTreeU, pCurveTree );
		}
		break;
	case COL:
		m_tess.m_nArrayHeight = nArrayLength;
		std::swap( m_tess.m_arrayHeight, arrayLength );

		if ( !m_patchDef3 ) {
			std::swap( m_tess.m_curveTreeV, pCurveTree );
		}
		break;
	}
}

inline void vertex_assign_ctrl( ArbitraryMeshVertex& vertex, const PatchControl& ctrl ){
	vertex.vertex = vertex3f_for_vector3( ctrl.m_vertex );
	vertex.texcoord = texcoord2f_for_vector2( ctrl.m_texcoord );
}

inline void vertex_clear_normal( ArbitraryMeshVertex& vertex ){
	vertex.normal = Normal3f( 0, 0, 0 );
	vertex.tangent = Normal3f( 0, 0, 0 );
	vertex.bitangent = Normal3f( 0, 0, 0 );
}

inline void tangents_remove_degenerate( Vector3 tangents[6], Vector2 textureTangents[6], unsigned int flags ){
	if ( flags & DEGEN_0a ) {
		const std::size_t i =
		    ( flags & DEGEN_0b )
		    ? ( flags & DEGEN_1a )
		      ? ( flags & DEGEN_1b )
		        ? ( flags & DEGEN_2a )
		          ? 5
		          : 4
		        : 3
		      : 2
		    : 1;
		tangents[0] = tangents[i];
		textureTangents[0] = textureTangents[i];
	}
	if ( flags & DEGEN_0b ) {
		const std::size_t i =
		    ( flags & DEGEN_0a )
		    ? ( flags & DEGEN_1b )
		      ? ( flags & DEGEN_1a )
		        ? ( flags & DEGEN_2b )
		          ? 4
		          : 5
		        : 2
		      : 3
		    : 0;
		tangents[1] = tangents[i];
		textureTangents[1] = textureTangents[i];
	}
	if ( flags & DEGEN_2a ) {
		const std::size_t i =
		    ( flags & DEGEN_2b )
		    ? ( flags & DEGEN_1a )
		      ? ( flags & DEGEN_1b )
		        ? ( flags & DEGEN_0a )
		          ? 1
		          : 0
		        : 3
		      : 2
		    : 5;
		tangents[4] = tangents[i];
		textureTangents[4] = textureTangents[i];
	}
	if ( flags & DEGEN_2b ) {
		const std::size_t i =
		    ( flags & DEGEN_2a )
		    ? ( flags & DEGEN_1b )
		      ? ( flags & DEGEN_1a )
		        ? ( flags & DEGEN_0b )
		          ? 0
		          : 1
		        : 2
		      : 3
		    : 4;
		tangents[5] = tangents[i];
		textureTangents[5] = textureTangents[i];
	}
}

void bestTangents00( unsigned int degenerateFlags, double dot, double length, std::size_t& index0, std::size_t& index1 ){
	if ( fabs( dot + length ) < 0.001 ) { // opposing direction = degenerate
		if ( !( degenerateFlags & DEGEN_1a ) ) { // if this tangent is degenerate we cannot use it
			index0 = 2;
			index1 = 0;
		}
		else if ( !( degenerateFlags & DEGEN_0b ) ) {
			index0 = 0;
			index1 = 1;
		}
		else
		{
			index0 = 1;
			index1 = 0;
		}
	}
	else if ( fabs( dot - length ) < 0.001 ) { // same direction = degenerate
		if ( degenerateFlags & DEGEN_0b ) {
			index0 = 0;
			index1 = 1;
		}
		else
		{
			index0 = 1;
			index1 = 0;
		}
	}
}

void bestTangents01( unsigned int degenerateFlags, double dot, double length, std::size_t& index0, std::size_t& index1 ){
	if ( fabs( dot - length ) < 0.001 ) { // same direction = degenerate
		if ( !( degenerateFlags & DEGEN_1a ) ) { // if this tangent is degenerate we cannot use it
			index0 = 2;
			index1 = 1;
		}
		else if ( !( degenerateFlags & DEGEN_2b ) ) {
			index0 = 4;
			index1 = 0;
		}
		else
		{
			index0 = 5;
			index1 = 1;
		}
	}
	else if ( fabs( dot + length ) < 0.001 ) { // opposing direction = degenerate
		if ( degenerateFlags & DEGEN_2b ) {
			index0 = 4;
			index1 = 0;
		}
		else
		{
			index0 = 5;
			index1 = 1;
		}
	}
}

void bestTangents10( unsigned int degenerateFlags, double dot, double length, std::size_t& index0, std::size_t& index1 ){
	if ( fabs( dot - length ) < 0.001 ) { // same direction = degenerate
		if ( !( degenerateFlags & DEGEN_1b ) ) { // if this tangent is degenerate we cannot use it
			index0 = 3;
			index1 = 4;
		}
		else if ( !( degenerateFlags & DEGEN_0a ) ) {
			index0 = 1;
			index1 = 5;
		}
		else
		{
			index0 = 0;
			index1 = 4;
		}
	}
	else if ( fabs( dot + length ) < 0.001 ) { // opposing direction = degenerate
		if ( degenerateFlags & DEGEN_0a ) {
			index0 = 1;
			index1 = 5;
		}
		else
		{
			index0 = 0;
			index1 = 4;
		}
	}
}

void bestTangents11( unsigned int degenerateFlags, double dot, double length, std::size_t& index0, std::size_t& index1 ){
	if ( fabs( dot + length ) < 0.001 ) { // opposing direction = degenerate
		if ( !( degenerateFlags & DEGEN_1b ) ) { // if this tangent is degenerate we cannot use it
			index0 = 3;
			index1 = 5;
		}
		else if ( !( degenerateFlags & DEGEN_2a ) ) {
			index0 = 5;
			index1 = 4;
		}
		else
		{
			index0 = 4;
			index1 = 5;
		}
	}
	else if ( fabs( dot - length ) < 0.001 ) { // same direction = degenerate
		if ( degenerateFlags & DEGEN_2a ) {
			index0 = 5;
			index1 = 4;
		}
		else
		{
			index0 = 4;
			index1 = 5;
		}
	}
}

void Patch::accumulateVertexTangentSpace( std::size_t index, Vector3 tangentX[6], Vector3 tangentY[6], Vector2 tangentS[6], Vector2 tangentT[6], std::size_t index0, std::size_t index1 ){
	{
		Vector3 normal( vector3_cross( tangentX[index0], tangentY[index1] ) );
		if ( !vector3_equal( normal, g_vector3_identity ) ) {
			vector3_add( normal_for_index( m_tess.m_vertices, index ), vector3_normalised( normal ) );
		}
	}

	{
		ArbitraryMeshVertex a, b, c;
		a.vertex = Vertex3f( 0, 0, 0 );
		a.texcoord = TexCoord2f( 0, 0 );
		b.vertex = vertex3f_for_vector3( tangentX[index0] );
		b.texcoord = texcoord2f_for_vector2( tangentS[index0] );
		c.vertex = vertex3f_for_vector3( tangentY[index1] );
		c.texcoord = texcoord2f_for_vector2( tangentT[index1] );

		Vector3 s, t;
		ArbitraryMeshTriangle_calcTangents( a, b, c, s, t );
		if ( !vector3_equal( s, g_vector3_identity ) ) {
			vector3_add( tangent_for_index( m_tess.m_vertices, index ), vector3_normalised( s ) );
		}
		if ( !vector3_equal( t, g_vector3_identity ) ) {
			vector3_add( bitangent_for_index( m_tess.m_vertices, index ), vector3_normalised( t ) );
		}
	}
}

const std::size_t PATCH_MAX_VERTEX_ARRAY = 1048576;

void Patch::BuildVertexArray(){
	const std::size_t strideU = 1;
	const std::size_t strideV = m_width;

	const std::size_t numElems = m_tess.m_nArrayWidth * m_tess.m_nArrayHeight; // total number of elements in vertex array

	const bool bWidthStrips = ( m_tess.m_nArrayWidth >= m_tess.m_nArrayHeight ); // decide if horizontal strips are longer than vertical


	// allocate vertex, normal, texcoord and primitive-index arrays
	m_tess.m_vertices.resize( numElems );
	m_tess.m_indices.resize( m_tess.m_nArrayWidth * 2 * ( m_tess.m_nArrayHeight - 1 ) );

	// set up strip indices
	if ( bWidthStrips ) {
		m_tess.m_numStrips = m_tess.m_nArrayHeight - 1;
		m_tess.m_lenStrips = m_tess.m_nArrayWidth * 2;

		for ( std::size_t i = 0; i < m_tess.m_nArrayWidth; i++ )
		{
			for ( std::size_t j = 0; j < m_tess.m_numStrips; j++ )
			{
				m_tess.m_indices[( j * m_tess.m_lenStrips ) + i * 2] = RenderIndex( j * m_tess.m_nArrayWidth + i );
				m_tess.m_indices[( j * m_tess.m_lenStrips ) + i * 2 + 1] = RenderIndex( ( j + 1 ) * m_tess.m_nArrayWidth + i );
				// reverse because radiant uses CULL_FRONT
				//m_tess.m_indices[( j * m_tess.m_lenStrips ) + i * 2 + 1] = RenderIndex( j * m_tess.m_nArrayWidth + i );
				//m_tess.m_indices[( j * m_tess.m_lenStrips ) + i * 2] = RenderIndex( ( j + 1 ) * m_tess.m_nArrayWidth + i );
			}
		}
	}
	else
	{
		m_tess.m_numStrips = m_tess.m_nArrayWidth - 1;
		m_tess.m_lenStrips = m_tess.m_nArrayHeight * 2;

		for ( std::size_t i = 0; i < m_tess.m_nArrayHeight; i++ )
		{
			for ( std::size_t j = 0; j < m_tess.m_numStrips; j++ )
			{
				m_tess.m_indices[( j * m_tess.m_lenStrips ) + i * 2] = RenderIndex( ( ( m_tess.m_nArrayHeight - 1 ) - i ) * m_tess.m_nArrayWidth + j );
				m_tess.m_indices[( j * m_tess.m_lenStrips ) + i * 2 + 1] = RenderIndex( ( ( m_tess.m_nArrayHeight - 1 ) - i ) * m_tess.m_nArrayWidth + j + 1 );
				// reverse because radiant uses CULL_FRONT
				//m_tess.m_indices[( j * m_tess.m_lenStrips ) + i * 2 + 1] = RenderIndex( ( ( m_tess.m_nArrayHeight - 1 ) - i ) * m_tess.m_nArrayWidth + j );
				//m_tess.m_indices[( j * m_tess.m_lenStrips ) + i * 2 ] = RenderIndex( ( ( m_tess.m_nArrayHeight - 1 ) - i ) * m_tess.m_nArrayWidth + j + 1 );

			}
		}
	}

	{
		PatchControlIter pCtrl = m_ctrlTransformed.data();
		for ( std::size_t j = 0, offStartY = 0; j + 1 < m_height; j += 2, pCtrl += ( strideU + strideV ) )
		{
			// set up array offsets for this sub-patch
			const bool leafY = ( m_patchDef3 ) ? false : BezierCurveTree_isLeaf( m_tess.m_curveTreeV[j >> 1] );
			const std::size_t offMidY = ( m_patchDef3 ) ? 0 : m_tess.m_curveTreeV[j >> 1]->index;
			const std::size_t widthY = m_tess.m_arrayHeight[j >> 1] * m_tess.m_nArrayWidth;
			const std::size_t offEndY = offStartY + widthY;

			for ( std::size_t i = 0, offStartX = 0; i + 1 < m_width; i += 2, pCtrl += ( strideU << 1 ) )
			{
				const bool leafX = ( m_patchDef3 ) ? false : BezierCurveTree_isLeaf( m_tess.m_curveTreeU[i >> 1] );
				const std::size_t offMidX = ( m_patchDef3 ) ? 0 : m_tess.m_curveTreeU[i >> 1]->index;
				const std::size_t widthX = m_tess.m_arrayWidth[i >> 1];
				const std::size_t offEndX = offStartX + widthX;

				PatchControl *subMatrix[3][3];
				subMatrix[0][0] = pCtrl;
				subMatrix[0][1] = subMatrix[0][0] + strideU;
				subMatrix[0][2] = subMatrix[0][1] + strideU;
				subMatrix[1][0] = subMatrix[0][0] + strideV;
				subMatrix[1][1] = subMatrix[1][0] + strideU;
				subMatrix[1][2] = subMatrix[1][1] + strideU;
				subMatrix[2][0] = subMatrix[1][0] + strideV;
				subMatrix[2][1] = subMatrix[2][0] + strideU;
				subMatrix[2][2] = subMatrix[2][1] + strideU;

				// assign on-patch control points to vertex array
				if ( i == 0 && j == 0 ) {
					vertex_clear_normal( m_tess.m_vertices[offStartX + offStartY] );
				}
				vertex_assign_ctrl( m_tess.m_vertices[offStartX + offStartY], *subMatrix[0][0] );
				if ( j == 0 ) {
					vertex_clear_normal( m_tess.m_vertices[offEndX + offStartY] );
				}
				vertex_assign_ctrl( m_tess.m_vertices[offEndX + offStartY], *subMatrix[0][2] );
				if ( i == 0 ) {
					vertex_clear_normal( m_tess.m_vertices[offStartX + offEndY] );
				}
				vertex_assign_ctrl( m_tess.m_vertices[offStartX + offEndY], *subMatrix[2][0] );

				vertex_clear_normal( m_tess.m_vertices[offEndX + offEndY] );
				vertex_assign_ctrl( m_tess.m_vertices[offEndX + offEndY], *subMatrix[2][2] );

				if ( !m_patchDef3 ) {
					// assign remaining control points to vertex array
					if ( !leafX ) {
						vertex_assign_ctrl( m_tess.m_vertices[offMidX + offStartY], *subMatrix[0][1] );
						vertex_assign_ctrl( m_tess.m_vertices[offMidX + offEndY], *subMatrix[2][1] );
					}
					if ( !leafY ) {
						vertex_assign_ctrl( m_tess.m_vertices[offStartX + offMidY], *subMatrix[1][0] );
						vertex_assign_ctrl( m_tess.m_vertices[offEndX + offMidY], *subMatrix[1][2] );

						if ( !leafX ) {
							vertex_assign_ctrl( m_tess.m_vertices[offMidX + offMidY], *subMatrix[1][1] );
						}
					}
				}

				// test all 12 edges for degeneracy
				unsigned int nFlagsX = subarray_get_degen( pCtrl, strideU, strideV );
				unsigned int nFlagsY = subarray_get_degen( pCtrl, strideV, strideU );
				Vector3 tangentX[6], tangentY[6];
				Vector2 tangentS[6], tangentT[6];

				// set up tangents for each of the 12 edges if they were not degenerate
				if ( !( nFlagsX & DEGEN_0a ) ) {
					tangentX[0] = vector3_subtracted( subMatrix[0][1]->m_vertex, subMatrix[0][0]->m_vertex );
					tangentS[0] = vector2_subtracted( subMatrix[0][1]->m_texcoord, subMatrix[0][0]->m_texcoord );
				}
				if ( !( nFlagsX & DEGEN_0b ) ) {
					tangentX[1] = vector3_subtracted( subMatrix[0][2]->m_vertex, subMatrix[0][1]->m_vertex );
					tangentS[1] = vector2_subtracted( subMatrix[0][2]->m_texcoord, subMatrix[0][1]->m_texcoord );
				}
				if ( !( nFlagsX & DEGEN_1a ) ) {
					tangentX[2] = vector3_subtracted( subMatrix[1][1]->m_vertex, subMatrix[1][0]->m_vertex );
					tangentS[2] = vector2_subtracted( subMatrix[1][1]->m_texcoord, subMatrix[1][0]->m_texcoord );
				}
				if ( !( nFlagsX & DEGEN_1b ) ) {
					tangentX[3] = vector3_subtracted( subMatrix[1][2]->m_vertex, subMatrix[1][1]->m_vertex );
					tangentS[3] = vector2_subtracted( subMatrix[1][2]->m_texcoord, subMatrix[1][1]->m_texcoord );
				}
				if ( !( nFlagsX & DEGEN_2a ) ) {
					tangentX[4] = vector3_subtracted( subMatrix[2][1]->m_vertex, subMatrix[2][0]->m_vertex );
					tangentS[4] = vector2_subtracted( subMatrix[2][1]->m_texcoord, subMatrix[2][0]->m_texcoord );
				}
				if ( !( nFlagsX & DEGEN_2b ) ) {
					tangentX[5] = vector3_subtracted( subMatrix[2][2]->m_vertex, subMatrix[2][1]->m_vertex );
					tangentS[5] = vector2_subtracted( subMatrix[2][2]->m_texcoord, subMatrix[2][1]->m_texcoord );
				}

				if ( !( nFlagsY & DEGEN_0a ) ) {
					tangentY[0] = vector3_subtracted( subMatrix[1][0]->m_vertex, subMatrix[0][0]->m_vertex );
					tangentT[0] = vector2_subtracted( subMatrix[1][0]->m_texcoord, subMatrix[0][0]->m_texcoord );
				}
				if ( !( nFlagsY & DEGEN_0b ) ) {
					tangentY[1] = vector3_subtracted( subMatrix[2][0]->m_vertex, subMatrix[1][0]->m_vertex );
					tangentT[1] = vector2_subtracted( subMatrix[2][0]->m_texcoord, subMatrix[1][0]->m_texcoord );
				}
				if ( !( nFlagsY & DEGEN_1a ) ) {
					tangentY[2] = vector3_subtracted( subMatrix[1][1]->m_vertex, subMatrix[0][1]->m_vertex );
					tangentT[2] = vector2_subtracted( subMatrix[1][1]->m_texcoord, subMatrix[0][1]->m_texcoord );
				}
				if ( !( nFlagsY & DEGEN_1b ) ) {
					tangentY[3] = vector3_subtracted( subMatrix[2][1]->m_vertex, subMatrix[1][1]->m_vertex );
					tangentT[3] = vector2_subtracted( subMatrix[2][1]->m_texcoord, subMatrix[1][1]->m_texcoord );
				}
				if ( !( nFlagsY & DEGEN_2a ) ) {
					tangentY[4] = vector3_subtracted( subMatrix[1][2]->m_vertex, subMatrix[0][2]->m_vertex );
					tangentT[4] = vector2_subtracted( subMatrix[1][2]->m_texcoord, subMatrix[0][2]->m_texcoord );
				}
				if ( !( nFlagsY & DEGEN_2b ) ) {
					tangentY[5] = vector3_subtracted( subMatrix[2][2]->m_vertex, subMatrix[1][2]->m_vertex );
					tangentT[5] = vector2_subtracted( subMatrix[2][2]->m_texcoord, subMatrix[1][2]->m_texcoord );
				}

				// set up remaining edge tangents by borrowing the tangent from the closest parallel non-degenerate edge
				tangents_remove_degenerate( tangentX, tangentS, nFlagsX );
				tangents_remove_degenerate( tangentY, tangentT, nFlagsY );

				{
					// x=0, y=0
					std::size_t index = offStartX + offStartY;
					std::size_t index0 = 0;
					std::size_t index1 = 0;

					double dot = vector3_dot( tangentX[index0], tangentY[index1] );
					double length = vector3_length( tangentX[index0] ) * vector3_length( tangentY[index1] );

					bestTangents00( nFlagsX, dot, length, index0, index1 );

					accumulateVertexTangentSpace( index, tangentX, tangentY, tangentS, tangentT, index0, index1 );
				}

				{
					// x=1, y=0
					std::size_t index = offEndX + offStartY;
					std::size_t index0 = 1;
					std::size_t index1 = 4;

					double dot = vector3_dot( tangentX[index0], tangentY[index1] );
					double length = vector3_length( tangentX[index0] ) * vector3_length( tangentY[index1] );

					bestTangents10( nFlagsX, dot, length, index0, index1 );

					accumulateVertexTangentSpace( index, tangentX, tangentY, tangentS, tangentT, index0, index1 );
				}

				{
					// x=0, y=1
					std::size_t index = offStartX + offEndY;
					std::size_t index0 = 4;
					std::size_t index1 = 1;

					double dot = vector3_dot( tangentX[index0], tangentY[index1] );
					double length = vector3_length( tangentX[index1] ) * vector3_length( tangentY[index1] );

					bestTangents01( nFlagsX, dot, length, index0, index1 );

					accumulateVertexTangentSpace( index, tangentX, tangentY, tangentS, tangentT, index0, index1 );
				}

				{
					// x=1, y=1
					std::size_t index = offEndX + offEndY;
					std::size_t index0 = 5;
					std::size_t index1 = 5;

					double dot = vector3_dot( tangentX[index0], tangentY[index1] );
					double length = vector3_length( tangentX[index0] ) * vector3_length( tangentY[index1] );

					bestTangents11( nFlagsX, dot, length, index0, index1 );

					accumulateVertexTangentSpace( index, tangentX, tangentY, tangentS, tangentT, index0, index1 );
				}

				//normalise normals that won't be accumulated again
				if ( i != 0 || j != 0 ) {
					normalise_safe( normal_for_index( m_tess.m_vertices, offStartX + offStartY ) );
					normalise_safe( tangent_for_index( m_tess.m_vertices, offStartX + offStartY ) );
					normalise_safe( bitangent_for_index( m_tess.m_vertices, offStartX + offStartY ) );
				}
				if ( i + 3 == m_width ) {
					normalise_safe( normal_for_index( m_tess.m_vertices, offEndX + offStartY ) );
					normalise_safe( tangent_for_index( m_tess.m_vertices, offEndX + offStartY ) );
					normalise_safe( bitangent_for_index( m_tess.m_vertices, offEndX + offStartY ) );
				}
				if ( j + 3 == m_height ) {
					normalise_safe( normal_for_index( m_tess.m_vertices, offStartX + offEndY ) );
					normalise_safe( tangent_for_index( m_tess.m_vertices, offStartX + offEndY ) );
					normalise_safe( bitangent_for_index( m_tess.m_vertices, offStartX + offEndY ) );
				}
				if ( i + 3 == m_width && j + 3 == m_height ) {
					normalise_safe( normal_for_index( m_tess.m_vertices, offEndX + offEndY ) );
					normalise_safe( tangent_for_index( m_tess.m_vertices, offEndX + offEndY ) );
					normalise_safe( bitangent_for_index( m_tess.m_vertices, offEndX + offEndY ) );
				}

				// set flags to average normals between shared edges
				if ( j != 0 ) {
					nFlagsX |= AVERAGE;
				}
				if ( i != 0 ) {
					nFlagsY |= AVERAGE;
				}
				// set flags to save evaluating shared edges twice
				nFlagsX |= SPLIT;
				nFlagsY |= SPLIT;

				// if the patch is curved.. tesselate recursively
				// use the relevant control curves for this sub-patch
				if ( m_patchDef3 ) {
					TesselateSubMatrixFixed( m_tess.m_vertices.data() + offStartX + offStartY, 1, m_tess.m_nArrayWidth, nFlagsX, nFlagsY, subMatrix );
				}
				else
				{
					if ( !leafX ) {
						TesselateSubMatrix( m_tess.m_curveTreeU[i >> 1], m_tess.m_curveTreeV[j >> 1],
						                    offStartX, offStartY, offEndX, offEndY, // array offsets
						                    nFlagsX, nFlagsY,
						                    subMatrix[1][0]->m_vertex, subMatrix[1][1]->m_vertex, subMatrix[1][2]->m_vertex,
						                    subMatrix[1][0]->m_texcoord, subMatrix[1][1]->m_texcoord, subMatrix[1][2]->m_texcoord,
						                    false );
					}
					else if ( !leafY ) {
						TesselateSubMatrix( m_tess.m_curveTreeV[j >> 1], m_tess.m_curveTreeU[i >> 1],
						                    offStartY, offStartX, offEndY, offEndX, // array offsets
						                    nFlagsY, nFlagsX,
						                    subMatrix[0][1]->m_vertex, subMatrix[1][1]->m_vertex, subMatrix[2][1]->m_vertex,
						                    subMatrix[0][1]->m_texcoord, subMatrix[1][1]->m_texcoord, subMatrix[2][1]->m_texcoord,
						                    true );
					}
				}

				offStartX = offEndX;
			}
			offStartY = offEndY;
		}
	}
}


Vector3 getAverageNormal( const Vector3& normal1, const Vector3& normal2 )
{
	// Beware of normals with 0 length
	if ( vector3_length_squared( normal1 ) == 0 ) return normal2;
	if ( vector3_length_squared( normal2 ) == 0 ) return normal1;

	// Both normals have length > 0
	//Vector3 n1 = vector3_normalised( normal1 );
	//Vector3 n2 = vector3_normalised( normal2 );

	// Get the angle bisector
	if( vector3_length_squared( normal1 + normal2 ) == 0 ) return normal1;

	Vector3 normal = vector3_normalised( normal1 + normal2 );

	// Now calculate the length correction out of the angle
	// of the two normals
	/* float factor = cos(n1.angle(n2) * 0.5); */
	float factor = vector3_dot( normal1, normal2 );
	if ( factor > 1.0 ) factor = 1;
	if ( factor < -1.0 ) factor = -1;
	factor = acos( factor );

	factor = cos( factor * 0.5 );

	// Check for div by zero (if the normals are antiparallel)
	// and stretch the resulting normal, if necessary
	if ( factor != 0 )
	{
		normal /= factor;
	}

	return normal;
}

void Patch::createThickenedOpposite( const Patch& sourcePatch,
                                     const float thickness,
                                     const int axis,
                                     bool& no12,
                                     bool& no34 )
{
	// Clone the dimensions from the other patch
	setDims( sourcePatch.getWidth(), sourcePatch.getHeight() );

	// Also inherit the tesselation from the source patch
	//setFixedSubdivisions( sourcePatch.subdivionsFixed(), sourcePatch.getSubdivisions() );

	// Copy the shader from the source patch
	SetShader( sourcePatch.GetShader() );

	// if extrudeAxis == 0,0,0 the patch is extruded along its vertex normals
	Vector3 extrudeAxis( 0 );

	switch ( axis ) {
	case 0: // X-Axis
		extrudeAxis = Vector3( 1, 0, 0 );
		break;
	case 1: // Y-Axis
		extrudeAxis = Vector3( 0, 1, 0 );
		break;
	case 2: // Z-Axis
		extrudeAxis = Vector3( 0, 0, 1 );
		break;
	default:
		// Default value already set during initialisation
		break;
	}

	//check if certain seams are required + cycling in normals calculation is needed
	//( endpoints != startpoints ) - not a cylinder or something
	for ( std::size_t col = 0; col < m_width; col++ ){
		if( vector3_length_squared( sourcePatch.ctrlAt( 0, col ).m_vertex - sourcePatch.ctrlAt( m_height - 1, col ).m_vertex ) > 0.1f ){
			//globalOutputStream() << "yes12.\n";
			no12 = false;
			break;
		}
	}
	for ( std::size_t row = 0; row < m_height; row++ ){
		if( vector3_length_squared( sourcePatch.ctrlAt( row, 0 ).m_vertex - sourcePatch.ctrlAt( row, m_width - 1 ).m_vertex ) > 0.1f ){
			no34 = false;
			//globalOutputStream() << "yes34.\n";
			break;
		}
	}

	for ( std::size_t col = 0; col < m_width; col++ )
	{
		for ( std::size_t row = 0; row < m_height; row++ )
		{
			// The current control vertex on the other patch
			const PatchControl& curCtrl = sourcePatch.ctrlAt( row, col );

			Vector3 normal;

			// Are we extruding along vertex normals (i.e. extrudeAxis == 0,0,0)?
			if ( extrudeAxis == Vector3( 0 ) )
			{
				// The col tangents (empty if 0,0,0)
				Vector3 colTangent[2] = { Vector3( 0 ), Vector3( 0 ) };

				// Are we at the beginning/end of the row? + not cylinder
				if ( ( col == 0 || col == m_width - 1 ) && !no34 )
				{
					// Get the next col index
					std::size_t nextCol = ( col == m_width - 1 ) ? ( col - 1 ) : ( col + 1 );

					const PatchControl& colNeighbour = sourcePatch.ctrlAt( row, nextCol );

					// One available tangent
					colTangent[0] = colNeighbour.m_vertex - curCtrl.m_vertex;
					// Reverse it if we're at the end of the column
					colTangent[0] *= ( col == m_width - 1 ) ? -1 : +1;
					//normalize
					if ( vector3_length_squared( colTangent[0] ) != 0 ) vector3_normalise( colTangent[0] );
				}
				// We are in between, two tangents can be calculated
				else
				{
					// Take two neighbouring vertices that should form a line segment
					std::size_t nextCol, prevCol;
					if( col == 0 ){
						nextCol = col+1;
						prevCol = m_width-2;
					}
					else if( col == m_width - 1 ){
						nextCol = 1;
						prevCol = col-1;
					}
					else{
						nextCol = col+1;
						prevCol = col-1;
					}
					const PatchControl& neighbour1 = sourcePatch.ctrlAt( row, nextCol );
					const PatchControl& neighbour2 = sourcePatch.ctrlAt( row, prevCol );


					// Calculate both available tangents
					colTangent[0] = neighbour1.m_vertex - curCtrl.m_vertex;
					colTangent[1] = neighbour2.m_vertex - curCtrl.m_vertex;

					// Reverse the second one
					colTangent[1] *= -1;

					//normalize b4 stuff
					if ( vector3_length_squared( colTangent[0] ) != 0 ) vector3_normalise( colTangent[0] );
					if ( vector3_length_squared( colTangent[1] ) != 0 ) vector3_normalise( colTangent[1] );

					// Cull redundant tangents (parallel)
					if ( vector3_length_squared( colTangent[1] + colTangent[0] ) == 0 ||
					     vector3_length_squared( colTangent[1] - colTangent[0] ) == 0 ){
						colTangent[1] = Vector3( 0 );
					}
				}

				// Calculate the tangent vectors to the next row
				Vector3 rowTangent[2] = { Vector3( 0 ), Vector3( 0 ) };

				// Are we at the beginning or the end?
				if ( ( row == 0 || row == m_height - 1 ) && !no12 )
				{
					// Yes, only calculate one row tangent
					// Get the next row index
					std::size_t nextRow = ( row == m_height - 1 ) ? ( row - 1 ) : ( row + 1 );

					const PatchControl& rowNeighbour = sourcePatch.ctrlAt( nextRow, col );

					// First tangent
					rowTangent[0] = rowNeighbour.m_vertex - curCtrl.m_vertex;
					// Reverse it accordingly
					rowTangent[0] *= ( row == m_height - 1 ) ? -1 : +1;
					//normalize
					if ( vector3_length_squared( rowTangent[0] ) != 0 ) vector3_normalise( rowTangent[0] );
				}
				else
				{
					// Two tangents to calculate
					std::size_t nextRow, prevRow;
					if( row == 0 ){
						nextRow = row + 1;
						prevRow = m_height - 2;
					}
					else if( row == m_height - 1 ){
						nextRow = 1;
						prevRow = row - 1;
					}
					else{
						nextRow = row + 1;
						prevRow = row - 1;
					}
					const PatchControl& rowNeighbour1 = sourcePatch.ctrlAt( nextRow, col );
					const PatchControl& rowNeighbour2 = sourcePatch.ctrlAt( prevRow, col );

					// First tangent
					rowTangent[0] = rowNeighbour1.m_vertex - curCtrl.m_vertex;
					rowTangent[1] = rowNeighbour2.m_vertex - curCtrl.m_vertex;

					// Reverse the second one
					rowTangent[1] *= -1;

					//normalize b4 stuff
					if ( vector3_length_squared( rowTangent[0] ) != 0 ) vector3_normalise( rowTangent[0] );
					if ( vector3_length_squared( rowTangent[1] ) != 0 ) vector3_normalise( rowTangent[1] );

					// Cull redundant tangents (parallel)
					if ( vector3_length_squared( rowTangent[1] + rowTangent[0] ) == 0 ||
					     vector3_length_squared( rowTangent[1] - rowTangent[0] ) == 0 ){
						rowTangent[1] = Vector3( 0 );
					}
				}


				//clean parallel pairs...
				if ( vector3_length_squared( rowTangent[0] + colTangent[0] ) == 0 ||
				     vector3_length_squared( rowTangent[0] - colTangent[0] ) == 0 ){
					rowTangent[0] = Vector3( 0 );
				}
				if ( vector3_length_squared( rowTangent[1] + colTangent[1] ) == 0 ||
				     vector3_length_squared( rowTangent[1] - colTangent[1] ) == 0 ){
					rowTangent[1] = Vector3( 0 );
				}
				if ( vector3_length_squared( rowTangent[0] + colTangent[1] ) == 0 ||
				     vector3_length_squared( rowTangent[0] - colTangent[1] ) == 0 ){
					colTangent[1] = Vector3( 0 );
				}
				if ( vector3_length_squared( rowTangent[1] + colTangent[0] ) == 0 ||
				     vector3_length_squared( rowTangent[1] - colTangent[0] ) == 0 ){
					rowTangent[1] = Vector3( 0 );
				}

				//clean dummies
				if ( vector3_length_squared( colTangent[0] ) == 0 ){
					colTangent[0] = colTangent[1];
					colTangent[1] = Vector3( 0 );
				}
				if ( vector3_length_squared( rowTangent[0] ) == 0 ){
					rowTangent[0] = rowTangent[1];
					rowTangent[1] = Vector3( 0 );
				}
				if( vector3_length_squared( rowTangent[0] ) == 0 || vector3_length_squared( colTangent[0] ) == 0 ){
					normal = extrudeAxis;

				}
				else{
					// If two column + two row tangents are available, take the length-corrected average
					if ( ( fabs( colTangent[1][0] ) + fabs( colTangent[1][1] ) + fabs( colTangent[1][2] ) ) > 0 &&
					     ( fabs( rowTangent[1][0] ) + fabs( rowTangent[1][1] ) + fabs( rowTangent[1][2] ) ) > 0 )
					{
						// Two column normals to calculate
						Vector3 normal1 = vector3_normalised( vector3_cross( rowTangent[0], colTangent[0] ) );
						Vector3 normal2 = vector3_normalised( vector3_cross( rowTangent[1], colTangent[1] ) );

						normal = getAverageNormal( normal1, normal2 );
						/*globalOutputStream() << "0\n";
						globalOutputStream() << normal1 << '\n';
						globalOutputStream() << normal2 << '\n';
						globalOutputStream() << normal << '\n';*/

					}
					// If two column tangents are available, take the length-corrected average
					else if ( ( fabs( colTangent[1][0] ) + fabs( colTangent[1][1] ) + fabs( colTangent[1][2] ) ) > 0)
					{
						// Two column normals to calculate
						Vector3 normal1 = vector3_normalised( vector3_cross( rowTangent[0], colTangent[0] ) );
						Vector3 normal2 = vector3_normalised( vector3_cross( rowTangent[0], colTangent[1] ) );

						normal = getAverageNormal( normal1, normal2 );
						/*globalOutputStream() << "1\n";
						globalOutputStream() << normal1 << '\n';
						globalOutputStream() << normal2 << '\n';
						globalOutputStream() << normal << '\n';*/

					}
					else
					{
						// One column tangent available, maybe we have a second rowtangent?
						if ( ( fabs( rowTangent[1][0] ) + fabs( rowTangent[1][1] ) + fabs( rowTangent[1][2] ) ) > 0)
						{
							// Two row normals to calculate
							Vector3 normal1 = vector3_normalised( vector3_cross( rowTangent[0], colTangent[0] ) );
							Vector3 normal2 = vector3_normalised( vector3_cross( rowTangent[1], colTangent[0] ) );

							normal = getAverageNormal( normal1, normal2 );
							/*globalOutputStream() << "2\n";
							globalOutputStream() << rowTangent[0] << '\n';
							globalOutputStream() << colTangent[0] << '\n';
							globalOutputStream() << vector3_cross( rowTangent[0], colTangent[0]) << '\n';
							globalOutputStream() << normal1 << '\n';
							globalOutputStream() << normal2 << '\n';
							globalOutputStream() << normal << '\n';*/

						}
						else
						{
							if ( vector3_length_squared( vector3_cross( rowTangent[0], colTangent[0] ) ) > 0 ){
								normal = vector3_normalised( vector3_cross( rowTangent[0], colTangent[0] ) );
								/*globalOutputStream() << "3\n";
								globalOutputStream() << (float)vector3_length_squared( vector3_cross( rowTangent[0], colTangent[0] ) ) << '\n';
								globalOutputStream() << normal << '\n';*/
							}
							else{
								normal = extrudeAxis;
							}
						}
					}
				}
			}
			else
			{
				// Take the predefined extrude direction instead
				normal = extrudeAxis;
			}

			// Store the new coordinates into this patch at the current coords
			ctrlAt( row, col ).m_vertex = curCtrl.m_vertex + normal * thickness;

			// Clone the texture cooordinates of the source patch
			ctrlAt( row, col ).m_texcoord = curCtrl.m_texcoord;
		}
	}

	// Notify the patch about the change
	controlPointsChanged();
}

void Patch::createThickenedWall( const Patch& sourcePatch,
                                 const Patch& targetPatch,
                                 const int wallIndex )
{
	// Copy the shader from the source patch
	SetShader( sourcePatch.GetShader() );

	// The start and end control vertex indices
	int start = 0;
	int end = 0;
	// The increment (incr = 1 for the "long" edge, incr = width for the "short" edge)
	int incr = 1;

	// These are the target dimensions of this wall
	// The width is depending on which edge is "seamed".
	int cols = 0;
	int rows = 3;

	int sourceWidth = static_cast<int>( sourcePatch.getWidth() );
	int sourceHeight = static_cast<int>( sourcePatch.getHeight() );
/*
	bool sourceTesselationFixed = sourcePatch.subdivionsFixed();
	Subdivisions sourceTesselationX( sourcePatch.getSubdivisions().x(), 1 );
	Subdivisions sourceTesselationY( sourcePatch.getSubdivisions().y(), 1 );
*/
	// Determine which of the four edges have to be connected
	// and calculate the start, end & stepsize for the following loop
	switch ( wallIndex ) {
	case 0:
		cols = sourceWidth;
		start = 0;
		end = sourceWidth - 1;
		incr = 1;
		//setFixedSubdivisions( sourceTesselationFixed, sourceTesselationX );
		break;
	case 1:
		cols = sourceWidth;
		start = sourceWidth * ( sourceHeight - 1 );
		end = sourceWidth*sourceHeight - 1;
		incr = 1;
		//setFixedSubdivisions( sourceTesselationFixed, sourceTesselationX );
		break;
	case 2:
		cols = sourceHeight;
		start = 0;
		end = sourceWidth * ( sourceHeight - 1 );
		incr = sourceWidth;
		//setFixedSubdivisions( sourceTesselationFixed, sourceTesselationY );
		break;
	case 3:
		cols = sourceHeight;
		start = sourceWidth - 1;
		end = sourceWidth * sourceHeight - 1;
		incr = sourceWidth;
		//setFixedSubdivisions( sourceTesselationFixed, sourceTesselationY );
		break;
	}

	setDims( cols, rows );

	const PatchControlArray& sourceCtrl = sourcePatch.getControlPoints();
	const PatchControlArray& targetCtrl = targetPatch.getControlPoints();

	int col = 0;
	// Now go through the control vertices with these calculated stepsize
	for ( int idx = start; idx <= end; idx += incr, col++ ) {
		Vector3 sourceCoord = sourceCtrl[idx].m_vertex;
		Vector3 targetCoord = targetCtrl[idx].m_vertex;
		Vector3 middleCoord = ( sourceCoord + targetCoord ) / 2;

		// Now assign the vertex coordinates
		ctrlAt( 0, col ).m_vertex = sourceCoord;
		ctrlAt( 1, col ).m_vertex = middleCoord;
		ctrlAt( 2, col ).m_vertex = targetCoord;
	}

	if ( wallIndex == 0 || wallIndex == 3 ) {
		InvertMatrix();
	}

	// Notify the patch about the change
	controlPointsChanged();

	// Texture the patch "naturally"
	NaturalTexture();
}


class PatchFilterWrapper final : public Filter
{
	bool m_active;
	bool m_invert;
	PatchFilter& m_filter;
public:
	PatchFilterWrapper( PatchFilter& filter, bool invert ) :
		m_active( false ), // suppress uninitialized warning
		m_invert( invert ),
		m_filter( filter ){
	}
	void setActive( bool active ) override {
		m_active = active;
	}
	bool active(){
		return m_active;
	}
	bool filter( const Patch& patch ){
		return m_invert ^ m_filter.filter( patch );
	}
};


typedef std::list<PatchFilterWrapper> PatchFilters;
PatchFilters g_patchFilters;

void add_patch_filter( PatchFilter& filter, int mask, bool invert ){
	g_patchFilters.push_back( PatchFilterWrapper( filter, invert ) );
	GlobalFilterSystem().addFilter( g_patchFilters.back(), mask );
}

bool patch_filtered( Patch& patch ){
	for ( PatchFilters::iterator i = g_patchFilters.begin(); i != g_patchFilters.end(); ++i )
	{
		if ( ( *i ).active() && ( *i ).filter( patch ) ) {
			return true;
		}
	}
	return false;
}
