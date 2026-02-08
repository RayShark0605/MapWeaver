#ifndef MAP_WEAVER_CORE_MAP_LAYER_H
#define MAP_WEAVER_CORE_MAP_LAYER_H

#include <string>
#include <vector>

struct WmsOnlineResourceAttribute
{
	std::string xlinkHrefUtf8 = "";
};

struct WmsGetProperty
{
	WmsOnlineResourceAttribute onlineResource;
};

struct WmsPostProperty
{
	WmsOnlineResourceAttribute onlineResource;
};

struct WmsHttpProperty
{
	WmsGetProperty get;
	WmsPostProperty post;
};

struct WmsDcpTypeProperty
{
	WmsHttpProperty http;
};

struct WmsOperationType
{
	std::vector<std::string> formatsUtf8;
	std::vector<WmsDcpTypeProperty> dcpTypes;
	std::vector<std::string> allowedEncodingsUtf8;
};

struct WmsRequestProperty
{
	WmsOperationType getMap;
	WmsOperationType getFeatureInfo;
	WmsOperationType getTile;
	WmsOperationType getLegendGraphic;
};

struct WmsExceptionProperty
{
	std::vector<std::string> formatsUtf8;
};

struct WmsContactPersonPrimaryProperty
{
	std::string contactPersonUtf8 = "";
	std::string contactOrganizationUtf8 = "";
};

struct WmsContactAddressProperty
{
	std::string addressTypeUtf8 = "";
	std::string addressUtf8 = "";
	std::string cityUtf8 = "";
	std::string stateOrProvinceUtf8 = "";
	std::string postCodeUtf8 = "";
	std::string countryUtf8 = "";
};

struct WmsContactInformationProperty
{
	WmsContactPersonPrimaryProperty personPrimary;
	std::string positionUtf8 = "";
	WmsContactAddressProperty address;
	std::string voiceTelephoneUtf8 = "";
	std::string facsimileTelephoneUtf8 = "";
	std::string eMailAddressUtf8 = "";
};

struct WmsServiceProperty
{
	std::string titleUtf8 = "";
	std::string abstractUtf8 = "";
	std::vector<std::string> keywordsUtf8;
	WmsOnlineResourceAttribute onlineResource;
	WmsContactInformationProperty contactInformation;
	std::string feesUtf8 = "";
	std::string accessConstraintsUtf8 = "";
	size_t layerLimit = 0;
	size_t maxWidth = 0;
	size_t maxHeight = 0;
};















#endif