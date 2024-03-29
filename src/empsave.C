/*
 * empsave.C
 *
 * .bgeo to .emp converter
 *
 * Copyright (c) 2010 Van Aarde Krynauw.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "geo2emp.h"

#include <GB/GB_AttributeDefines.h>
#include <GEO/GEO_AttributeHandle.h>
#include <GEO/GEO_AttributeHandleList.h>
#include <GEO/GEO_TriMesh.h>

#include <NbEmpReader.h>
#include <NbFactory.h>
#include <NbString.h>

using namespace geo2emp;

/**************************************************************************************************/

Geo2Emp::ErrorCode Geo2Emp::saveMeshShape(std::list<Nb::Body*>& meshBodyList)
{
	const GEO_Primitive* pprim;
	const GEO_Point* ppt;
	GEO_AttributeHandleList attribList;
	GEO_AttributeHandle* pAttr;
	GB_Attribute* pBaseAttr;
	UT_Vector3 pos;
	Nb::Body* pMeshBody;
	std::string bodyName, attrname;
	int ptnum, totalpoints;
	int numAttribs;
	//Maps houdini attributes to some cached data
	std::map<int, AttributeInfo> attrLut;
	AttributeInfo* pAttrInfo;
	int attrSize;
	const GEO_PointAttribDict *pPtAttrDict;

	if (!_gdp)
	{
		//If we don't have a GDP for writing data into Houdini, return error.
		return EC_NULL_READ_GDP;
	}

	LogInfo() << " ************** Saving Mesh Shape ************** " << std::endl;	

	attribList.bindDetail(_gdp);
	
	//Add all the GDP point attributes to the attribute list
	attribList.appendAllAttributes( GEO_POINT_DICT );

	numAttribs = attribList.entries();

	LogVerbose() << "Number of attributes: " << attribList.entries() << " empty:" << attribList.isEmpty() << std::endl;	

	for (int i = 0; i < numAttribs; i++)
	{
		pAttr = attribList[i];
		pAttrInfo = &( attrLut[i] );
		pAttrInfo->entries = pAttr->entries();

		LogDebug() << "pAttr: " << pAttr << " - " << std::endl;
		LogDebug() << "Attribute #" << i << " - " << pAttr->getName() << " - " << attrSize << std::endl;

		//Note: if attribute overrides need to be done, this is probably the place.
		//The attribute creation code *might* need to be moved to another function...or at least some of it
		//so that it may be shared by other conversion code
		if ( pAttr->isP() )
		{
			//If we have position, then set up the type manually
			LogDebug() << "We have a position attribute. Set attributes manually." << std::endl;
			pAttrInfo->entries = 3;
			pAttrInfo->type = GB_ATTRIB_VECTOR;
		}
		else
		{
			//Retrieve attribute info through the standard channels...
			LogDebug() << "Retrieving attribute data using the Point Dictionary." << std::endl;

			//Now that we now which attribute we want, find it using the GU_Detail object.
			//TODO: Is there some beter way of doing this???
			pPtAttrDict = dynamic_cast< const GEO_PointAttribDict* >( _gdp->getAttributeDict( GEO_POINT_DICT ) );
			pBaseAttr = _gdp->getAttributeDict( GEO_POINT_DICT )->find( pAttr->getName() );
			pAttrInfo->type = pBaseAttr->getType();
		}

		LogDebug() << "Switching on attribute type." << std::endl;

	}

	//Build a map to separate bodies using their name attributes
	StringToPrimPolyListMap namesMap;
	StringToPrimPolyListMap::iterator nameIt;
	PrimPolyList::iterator polyIt;
	//Build a vector to remap point numbers.
	std::vector<int> remappedPtNum;

	buildTriPrimNamesMap( namesMap );	

	//Iterate over the names in the map
	for (nameIt = namesMap.begin(); nameIt != namesMap.end(); nameIt++)
	{
		LogInfo() << "Processing Name: " << nameIt->first << std::endl;		
		bodyName = nameIt->first;
		//List of polygons for this body
		PrimPolyList &polyList = nameIt->second;

		//Clear the remapped point numbers for this body
		remappedPtNum.clear();
		//remappedPtNum.resize( _gdp->primitives().entries()*3, -1 );
		remappedPtNum.resize( _gdp->points().entries(), -1 );

		//Create a Naiad mesh body
		pMeshBody = Nb::Factory::createBody("Mesh", bodyName);
		meshBodyList.push_back( pMeshBody );
	
		//get the mutable shapes for the mesh body.
		Nb::TriangleShape& triShape( pMeshBody->mutableTriangleShape() );
		Nb::PointShape& ptShape( pMeshBody->mutablePointShape() );

		Nb::Buffer3f& pPosBuf( ptShape.mutableBuffer3f("position") );
		pPosBuf.reserve( polyList.size() * 3 ); //Each primitive is treated as a triangle

		//This preprocessor defition was placed here simply because it is only applicable to this code section
		//and will fail to work anywhere else. Not to be classified under "best coding practices".
		#define PROCESSCHANNEL(type, defaults) \
		{ \
			if ( !ptShape.hasChannels ## type (empAttrName) ) \
			{ \
				ptShape.createChannel ## type ( empAttrName, (defaults) ); \
			} \
			pAttrInfo->empIndex = ptShape.channelIndex( empAttrName ); \
			ptShape.mutableBuffer ## type ( empAttrName ).reserve ( polyList.size() * 3 ); \
			pAttrInfo->supported = true; \
		}

		//Iterate over the BGEO attributes and create corresponding channels in the EMP
		//Also, store the channel index for direct lookups when transferring data.
		for (int i = 0; i < numAttribs; i++)
		{
			//Mangle the geo attribute name right now
			pAttr = attribList[i];
			std::string empAttrName;
			if ( _geoAttribMangle.find( pAttr->getName() ) != _geoAttribMangle.end() )
				empAttrName = _geoAttribMangle[ pAttr->getName() ];
			else
				empAttrName = pAttr->getName();
			LogDebug() << "Mangling name: " << pAttr->getName() << " ==> " << empAttrName << std::endl;
			pAttrInfo = &( attrLut[i] );
			pAttrInfo->supported = false;
			pAttrInfo->flipvector = true; //By default, flip vectors

			if ( std::string(pAttr->getName()).compare("P") == 0 )
				pAttrInfo->flipvector = false;

			pAttrInfo->empIndex = -1;
			switch ( pAttrInfo->type )
			{
				case GB_ATTRIB_FLOAT:
					LogDebug() << "Float " << pAttrInfo->entries << std::endl;
					switch ( pAttrInfo->entries )
					{
						case 1:
							PROCESSCHANNEL( 1f, 0.0f );
							break;
						case 3:
							PROCESSCHANNEL( 3f, em::vec3f(0.0f) );
							break;
					}
					break;
				case GB_ATTRIB_INT:
					LogDebug() << "Int " << pAttrInfo->entries << std::endl;
					break;
				case GB_ATTRIB_VECTOR:
					{
						LogDebug() << "Vector (Vector3)" << std::endl;
						PROCESSCHANNEL( 3f, em::vec3f(0.0f) );
						break;
					}
				case GB_ATTRIB_MIXED:
				case GB_ATTRIB_INDEX:
				default:
					//Unsupported attribute, so give it a skip.
					LogDebug() << "Unsupported attribute type for blind copy [" << pAttrInfo->type << "]" << std::endl;
					continue;
					break;
			}

			if ( pAttrInfo->supported )
				LogDebug() << "Supported Blind Copy! Channel: [" << empAttrName << "] Index: " << pAttrInfo->empIndex << std::endl;
			else
			{
				LogDebug() << "Unsupported Blind Copy! Channel: [" << empAttrName << "]" <<  std::endl;
				continue;
			}
		} //for numAttribs


		//Now its time to set the index channel to form faces
		Nb::Buffer3i& indexBuffer = triShape.mutableBuffer3i("index");
		indexBuffer.reserve( polyList.size() );
		em::vec3i triVec(0,0,0); //Triangle description for the index buffer
		totalpoints = 0;

		LogDebug() << "Starting poly iteration... " << std::endl;
		//Iterate over all the primitives in this list and remap their point numbers
		for ( polyIt = polyList.begin(); polyIt != polyList.end(); polyIt++ )
		{
			pprim = *polyIt;
			if ( pprim->getVertexCount() >= 3 )
			{
				//only extract the first three vertices from the primitive
				for (int i = 0; i < 3; i++)
				{
					//First iterate the points and make sure they have remapped point numbers
					//NOTE: Houdini & Naiad triangle windings differ.
					ppt = pprim->getVertex(2-i).getPt();
					ptnum = ppt->getNum();

					if (remappedPtNum.at( ptnum ) == -1)
					{
						//We have an unmapped point for this body
						//Push the applicable point attributes into each channel
						transferMeshPointAttribs(numAttribs, attribList, attrLut, ptShape, ppt);
												
						//Push the point into the position buffer
						
						//pos = ppt->getPos();
						//pPosBuf.push_back( em::vec3f( pos[0], pos[1], pos[2]) );

						//Record the position at which this point is stored
						remappedPtNum[ ptnum ] = totalpoints;
						totalpoints++;						
					}

					//At this point the current vertex has been remapped at some stage.
					//Write the remapped index in the triVec
					triVec[i] = remappedPtNum[ ptnum ];

					//Perform a blind copy of all the attributes.

				}
				//Push the remapped indices into the body's index buffer.
				indexBuffer.push_back( triVec );
			}
			else
			{
				//Not interested!
				LogVerbose() << "Warning! Primitive with less than 3 vertices found on body: " << bodyName << " (Vertex count: " << pprim->getVertexCount() << ")" << std::endl;
				continue;
			}
		}
	}

	LogInfo() << " ************** Done with Mesh Shape ************** " << std::endl;	

	return EC_SUCCESS;
}

/**************************************************************************************************/

void Geo2Emp::transferMeshPointAttribs(int numAttribs, GEO_AttributeHandleList& attribList, std::map<int, AttributeInfo>& attrLut, Nb::PointShape& ptShape, const GEO_Point* ppt)
{
	GEO_AttributeHandle* pAttr;
	AttributeInfo* pAttrInfo;

	for (int i = 0; i < numAttribs; i++)
	{
		pAttr = attribList[i];
		pAttrInfo = &( attrLut[i] );

		if (!pAttrInfo->supported)
			//Skip unsupported attributes
			continue;

		pAttr->setElement( ppt );
		if (! pAttr->isAttributeValid() )
		{
			LogDebug() << "Invalid attribute handle on supported attribute!! [" << pAttr->getName() << "]" << std::endl;
		}

		LogDebug() << "Transferring attribute: " << pAttr->getName() << std::endl;

		switch ( pAttrInfo->type )
		{
			case GB_ATTRIB_FLOAT:
				//LogDebug() << "Transfer Float" << pAttrInfo->entries << "[" << pAttr->getName() << "]" << std::endl;
				switch ( pAttrInfo->entries )
				{
					case 1:
						{
							//LogDebug() << "Float1: " << pAttr->getV3() << std::endl;
							//Get the channel from the point shape
							Nb::Buffer1f& pbuf = ptShape.mutableBuffer1f( pAttrInfo->empIndex );
							//Write the data into the buffer
							pbuf.push_back( pAttr->getF() );
						}
						break;
					case 3:
						{
							//LogDebug() << "Float3: " << pAttr->getV3() << std::endl;
							//Get the channel from the point shape
							Nb::Buffer3f& pbuf = ptShape.mutableBuffer3f( pAttrInfo->empIndex );
							//Write the data into the buffer
							pbuf.push_back( em::vec3f( pAttr->getF(0), pAttr->getF(1), pAttr->getF(2) ) );
						}
						break;
				}
				break;
			case GB_ATTRIB_INT:
				LogDebug() << "Int " << pAttrInfo->entries << std::endl;
				break;
			case GB_ATTRIB_VECTOR:
				{
					LogDebug() << "Transfer Vector3 [" << pAttr->getName() << "] " <<  pAttr->getF(0) << "," << pAttr->getF(1) << "," << pAttr->getF(2)<< std::endl;
					Nb::Buffer3f& pbuf = ptShape.mutableBuffer3f( pAttrInfo->empIndex );
					//If we have a vector, we need to invert it (reverse winding).
					//Write the data into the buffer
					pbuf.push_back( em::vec3f( pAttr->getF(0), pAttr->getF(1), pAttr->getF(2) ) );

					break;
				}
			case GB_ATTRIB_MIXED:
			case GB_ATTRIB_INDEX:
			default:
				//Unsupported attribute, so give it a skip.
				LogDebug() << " !!!!! SHOULDNT GET THIS !!!! Unsupported attribute type for blind copy [" << pAttrInfo->type << "]" << std::endl;
				continue;
				break;
		}

	}

}

/**************************************************************************************************/

void Geo2Emp::transferParticlePointAttribs(int numAttribs, GEO_AttributeHandleList& attribList, std::map<int, AttributeInfo>& attrLut, Nb::ParticleShape& shape, const GEO_Point* ppt)
{
	GEO_AttributeHandle* pAttr;
	AttributeInfo* pAttrInfo;

	for (int i = 0; i < numAttribs; i++)
	{
		pAttr = attribList[i];
		pAttrInfo = &( attrLut[i] );

		if (!pAttrInfo->supported)
			//Skip unsupported attributes
			continue;

		pAttr->setElement( ppt );
		if (! pAttr->isAttributeValid() )
		{
			LogDebug() << "Invalid attribute handle on supported attribute!! [" << pAttr->getName() << "]" << std::endl;
		}

		LogDebug() << "Transferring attribute: " << pAttr->getName() << std::endl;

		switch ( pAttrInfo->type )
		{
			case GB_ATTRIB_FLOAT:
				//LogDebug() << "Transfer Float" << pAttrInfo->entries << "[" << pAttr->getName() << "]" << std::endl;
				switch ( pAttrInfo->entries )
				{
					case 1:
						{
							//LogDebug() << "Float1: " << pAttr->getV3() << std::endl;
							//Get the channel from the point shape
							em::block3_array1f& vecData = shape.mutableBlocks1f( pAttrInfo->empIndex );
							//Write the data into the buffer
							vecData(0).push_back( pAttr->getF() );
						}
						break;
					case 3:
						{
							LogDebug() << "Float3: " << pAttr->getV3() << std::endl;
							//Get the channel from the particle shape
							em::block3_array3f& vecData = shape.mutableBlocks3f( pAttrInfo->empIndex );
							//Write the data into the buffer
							vecData(0).push_back( em::vec3f( pAttr->getF(0), pAttr->getF(1), pAttr->getF(2) ) );
						}
						break;
				}
				break;
			case GB_ATTRIB_INT:
				LogDebug() << "Int " << pAttrInfo->entries << std::endl;
				switch ( pAttrInfo->entries )
				{
					case 1:
						{
							//LogDebug() << "Float1: " << pAttr->getV3() << std::endl;
							//Get the channel from the point shape
							em::block3_array1i& vecData = shape.mutableBlocks1i( pAttrInfo->empIndex );
							//Write the data into the buffer
							vecData(0).push_back( pAttr->getI() );
						}
						break;
					case 3:
						{
							LogDebug() << "Int3: " << std::endl;
							//Get the channel from the particle shape
							em::block3_array3i& vecData = shape.mutableBlocks3i( pAttrInfo->empIndex );
							//Write the data into the buffer
							vecData(0).push_back( em::vec3i( pAttr->getI(0), pAttr->getI(1), pAttr->getI(2) ) );
						}
						break;
				}
				break;
			case GB_ATTRIB_VECTOR:
				{
					//LogDebug() << "Transfer Vector3 [" << pAttr->getName() << "] " <<  pAttr->getF(0) << "," << pAttr->getF(1) << "," << pAttr->getF(2)<< std::endl;
					//If we have a vector, we need to invert it (reverse winding).
					em::block3_array3f& vecData = shape.mutableBlocks3f( pAttrInfo->empIndex );
					//Write the data into the buffer
					vecData(0).push_back( em::vec3f( pAttr->getF(0), pAttr->getF(1), pAttr->getF(2) ) );
					break;
				}
			case GB_ATTRIB_MIXED:
			case GB_ATTRIB_INDEX:
			default:
				//Unsupported attribute, so give it a skip.
				LogDebug() << " !!!!! SHOULDNT GET THIS !!!! Unsupported attribute type for blind copy [" << pAttrInfo->type << "]" << std::endl;
				continue;
				break;
		}

	}

}
/**************************************************************************************************/

Geo2Emp::ErrorCode Geo2Emp::saveParticleShape(Nb::Body*& pParticleBody)
{
	if (!_gdp)
	{
		//If we don't have a GDP for writing data into Houdini, return error.
		return EC_NULL_READ_GDP;
	}

	LogInfo() << " ************** Saving Particle Shape ************** " << std::endl;	

	
	pParticleBody = Nb::Factory::createBody("Particle", _bodyName);
		
	//get the mutable shapes for the mesh body.
	Nb::ParticleShape& particleShape( pParticleBody->mutableParticleShape() );
	Nb::TileLayout& layout( pParticleBody->mutableLayout() );
	
	layout.worldBoxRefine( em::vec3f(0,0,0), em::vec3f(1,1,1),1,false);

	//Maps houdini attributes to some cached data
  GEO_AttributeHandleList attribList;
  GEO_AttributeHandle* pAttr;
	const GEO_PointList& ptlist = _gdp->points();
	int numpoints = ptlist.entries();
  GB_Attribute* pBaseAttr;
	std::map<int, AttributeInfo> attrLut;
	AttributeInfo* pAttrInfo;
	int attrSize;
	int numAttribs;
	const GEO_PointAttribDict *pPtAttrDict;

	attribList.bindDetail(_gdp);
	
	//Add all the GDP point attributes to the attribute list
	attribList.appendAllAttributes( GEO_POINT_DICT );

	numAttribs = attribList.entries();

	LogVerbose() << "Number of hou attributes: " << attribList.entries() << " empty:" << attribList.isEmpty() << std::endl;	

	for (int i = 0; i < numAttribs; i++)
	{
		pAttr = attribList[i];
		pAttrInfo = &( attrLut[i] );
		pAttrInfo->entries = pAttr->entries();

		LogDebug() << "pAttr: " << pAttr << " - " << std::endl;
		LogDebug() << "Attribute #" << i << " - " << pAttr->getName() << " [size]  " << attrSize << std::endl;

		//Note: if attribute overrides need to be done, this is probably the place.
		//The attribute creation code *might* need to be moved to another function...or at least some of it
		//so that it may be shared by other conversion code
		if ( pAttr->isP() )
		{
			//If we have position, then set up the type manually
			LogDebug() << "We have a position attribute. Set attributes manually." << std::endl;
			pAttrInfo->entries = 3;
			pAttrInfo->type = GB_ATTRIB_VECTOR;
		}
		else
		{
			//Retrieve attribute info through the standard channels...
			LogDebug() << "Retrieving attribute data using the Point Dictionary." << std::endl;

			//Now that we now which attribute we want, find it using the GU_Detail object.
			//TODO: Is there some beter way of doing this???
			pPtAttrDict = dynamic_cast< const GEO_PointAttribDict* >( _gdp->getAttributeDict( GEO_POINT_DICT ) );
			pBaseAttr = _gdp->getAttributeDict( GEO_POINT_DICT )->find( pAttr->getName() );
			pAttrInfo->type = pBaseAttr->getType();
		}

		LogDebug() << "Switching on attribute type." << std::endl;

	}


	//This preprocessor defition was placed here simply because it is only applicable to this code section
	//and will fail to work anywhere else. Not to be classified under "best coding practices".
	#define PROCESSCHANNEL_PARTICLE(type1, type2, defaults) \
	{ \
		particleShape.guaranteeChannel ## type1 (empAttrName); \
		pAttrInfo->empIndex = particleShape.channelIndex( empAttrName ); \
		em::block3_array ## type1 &vectorBlocks( particleShape.mutableBlocks ## type1 ( empAttrName ) ); \
		em::block3 ## type2 &vectorData( vectorBlocks(0) ); \
		vectorData.reserve( _gdp->points().entries() ); \
		pAttrInfo->supported = true; \
	}

	//Iterate over the BGEO attributes and create corresponding channels in the EMP body
	//Also, store the channel index for direct lookups when transferring data.
	for (int i = 0; i < numAttribs; i++)
	{
		//Mangle the geo attribute name right now
		pAttr = attribList[i];
		std::string empAttrName;
		if ( _geoAttribMangle.find( pAttr->getName() ) != _geoAttribMangle.end() )
			empAttrName = _geoAttribMangle[ pAttr->getName() ];
		else
			empAttrName = pAttr->getName();
		LogDebug() << "Mangling name: " << pAttr->getName() << " ==> " << empAttrName << std::endl;
		pAttrInfo = &( attrLut[i] );
		pAttrInfo->supported = false;
		pAttrInfo->flipvector = true; //By default, flip vectors

		if ( std::string(pAttr->getName()).compare("P") == 0 )
		{	
			pAttrInfo->flipvector = false;
		}

		pAttrInfo->empIndex = -1;
		switch ( pAttrInfo->type )
		{
			case GB_ATTRIB_FLOAT:
				LogDebug() << "Float " << pAttrInfo->entries << std::endl;
				switch ( pAttrInfo->entries )
				{
					case 1:
						PROCESSCHANNEL_PARTICLE( 1f, f, 0.0f );
						break;
					case 3:
						PROCESSCHANNEL_PARTICLE( 3f, vec3f, em::vec3f(0.0f) );
						break;
				}
				break;
			case GB_ATTRIB_INT:
				LogDebug() << "Int " << pAttrInfo->entries << std::endl;
				switch ( pAttrInfo->entries )
				{
					case 1:
						PROCESSCHANNEL_PARTICLE( 1i, i, 0 );
						break;
					case 3:
						PROCESSCHANNEL_PARTICLE( 3i, vec3i, em::vec3i(0.0f) );
						break;
				}
				break;
			case GB_ATTRIB_VECTOR:
				{
					LogDebug() << "Vector (Vector3)" << std::endl;
					if ( empAttrName.compare("position") == 0) 
					{ 
						//If this is position, then set the tile layout
						layout.pointRefine( particleShape.mutableBlocks3f("position") ); 
						particleShape.sync( layout ); 
					}
					PROCESSCHANNEL_PARTICLE( 3f, vec3f, em::vec3f(0.0f) );
					break;
				}
			case GB_ATTRIB_MIXED:
			case GB_ATTRIB_INDEX:
			default:
				//Unsupported attribute, so give it a skip.
				LogDebug() << "Unsupported attribute type for blind copy [" << pAttrInfo->type << "]" << std::endl;
				continue;
				break;
		}

		if ( pAttrInfo->supported )
			LogDebug() << "Supported Blind Copy! Channel: [" << empAttrName << "] Index: " << pAttrInfo->empIndex << std::endl;
		else
		{
			LogDebug() << "Unsupported Blind Copy! Channel: [" << empAttrName << "]" <<  std::endl;
			continue;
		}
	} //for numAttribs

	//Loop over the points and transfer attributes

	for (int i = 0; i < numpoints; i++)
	{
		transferParticlePointAttribs( numAttribs, attribList, attrLut, particleShape, ptlist[i] );
	}

	pParticleBody->update();

	return EC_SUCCESS;





	//Ng::TileLayout& layout( pParticleBody->mutableLayout() );
	
	// PLEASE NOTE: this should really be a box close to a particle, instead of (0,0,0)...(1,1,1) but I was in a hurry!
	// so this will make some "dummy" tiles...
	layout.worldBoxRefine( em::vec3f(0,0,0), em::vec3f(1,1,1),1,false);
	particleShape.sync( layout );

	//For the sake of simplicity, copy ALL the points in the GDP across.
	//It will be much slower if we have to filter out all the non-mesh vertices and then remap the vertices to new point numbers.
	UT_Vector3 pos, v3;
	GEO_AttributeHandle attr_v, attr_N;

	//The position attribute needs special/explicit treatment
	//Get the position buffer from the particleShape
	
	em::block3_array3f& vectorBlocks( particleShape.mutableBlocks3f("position") );
	em::block3vec3f& vectorData( vectorBlocks(0) );

	//Make sure we have enough space in the position attribute
	vectorData.reserve( _gdp->points().entries() );

	for (int i = 0; i < numpoints; i++)
	{
		//Set the point position
		pos = _gdp->points()[i]->getPos();
		vectorData.push_back( em::vec3f( pos[0], pos[1], pos[2] ) );
	}

	pParticleBody->update();

	LogInfo() << " ************** Done with Particle Shape ************** " << std::endl;	
	
	return EC_SUCCESS;
}

/**************************************************************************************************/

