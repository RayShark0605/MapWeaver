#include "GeoBoundingBox.h"
#include "GeoCrs.h"

const GeoBoundingBox GeoBoundingBox::Invalid = GeoBoundingBox();

GeoBoundingBox::GeoBoundingBox()
{
}

GeoBoundingBox::GeoBoundingBox(const std::string& wktUtf8) : wktUtf8(wktUtf8)
{
}

GeoBoundingBox::GeoBoundingBox(const std::string& wktUtf8, const GB_Rectangle& rect) : wktUtf8(wktUtf8), rect(rect)
{
	if (this->rect.IsValid())
	{
		this->rect.Normalize();
	}
}

GeoBoundingBox::~GeoBoundingBox()
{
}

bool GeoBoundingBox::operator==(const GeoBoundingBox& other) const
{
	if (wktUtf8 == other.wktUtf8)
	{
		return rect == other.rect;
	}

	const GeoCrs thisCrs = GeoCrs::CreateFromWkt(wktUtf8);
	const GeoCrs otherCrs = GeoCrs::CreateFromWkt(other.wktUtf8);
	if (thisCrs != otherCrs)
	{
		return false;
	}

	return rect == other.rect;
}

bool GeoBoundingBox::operator!=(const GeoBoundingBox& other) const
{
	return !(*this == other);
}

bool GeoBoundingBox::IsValid() const
{
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



