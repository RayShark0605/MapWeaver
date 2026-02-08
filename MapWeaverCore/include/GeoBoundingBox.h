#ifndef MAP_WEAVER_GEO_BOUNDINGBOX_H
#define MAP_WEAVER_GEO_BOUNDINGBOX_H

#include "MapWeaverPort.h"
#include "Geometry/GB_Rectangle.h"
#include <string>

#ifdef _MSC_VER
#  pragma warning(push)
#  pragma warning(disable: 4251)
#endif

class MAPWEAVERCORE_PORT GeoBoundingBox
{
public:
	std::string wktUtf8 = "";
	GB_Rectangle rect;

	static const GeoBoundingBox Invalid;

	GeoBoundingBox();
	explicit GeoBoundingBox(const std::string& wktUtf8);
	GeoBoundingBox(const std::string& wktUtf8, const GB_Rectangle& rect);
	virtual ~GeoBoundingBox();

	bool operator==(const GB_Rectangle& other) const;
	bool operator!=(const GB_Rectangle& other) const;

	bool IsValid() const;

	void Reset();

	void Set(const std::string& wktUtf8, const GB_Rectangle& rect);




	std::string SerializeToString() const;
	GB_ByteBuffer SerializeToBinary() const;

	bool Deserialize(const std::string& data);
	bool Deserialize(const GB_ByteBuffer& data);
};

#ifdef _MSC_VER
#  pragma warning(pop)
#endif









#endif