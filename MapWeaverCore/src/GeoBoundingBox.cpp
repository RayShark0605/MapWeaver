#include "GeoBoundingBox.h"

const GeoBoundingBox GeoBoundingBox::Invalid = GeoBoundingBox();

GeoBoundingBox::GeoBoundingBox()
{
}

GeoBoundingBox::GeoBoundingBox(const std::string& wktUtf8) : wktUtf8(wktUtf8)
{
}

GeoBoundingBox::GeoBoundingBox(const std::string& wktUtf8, const GB_Rectangle& rect) : wktUtf8(wktUtf8), rect(rect)
{
}

GeoBoundingBox::~GeoBoundingBox()
{
}

bool GeoBoundingBox::operator==(const GB_Rectangle& other) const
{


	return true;
}

bool GeoBoundingBox::operator!=(const GB_Rectangle& other) const
{


	return true;
}

bool GeoBoundingBox::IsValid() const
{

	return false;
}

void GeoBoundingBox::Reset()
{



}

void GeoBoundingBox::Set(const std::string& wktUtf8, const GB_Rectangle& rect)
{
	this->wktUtf8 = wktUtf8;
	this->rect = rect;
}

std::string GeoBoundingBox::SerializeToString() const
{
	return "";
}

GB_ByteBuffer GeoBoundingBox::SerializeToBinary() const
{
	return GB_ByteBuffer();
}

bool GeoBoundingBox::Deserialize(const std::string& data)
{

	return false;
}

bool GeoBoundingBox::Deserialize(const GB_ByteBuffer& data)
{

	return false;
}



