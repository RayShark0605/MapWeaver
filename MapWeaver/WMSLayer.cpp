#include "WMSLayer.h"
#include <unordered_set>

using namespace std;

WMSLayerAttribution::WMSLayerAttribution(const string& title, const string& xlinkHref) :title(title), xlinkHref(xlinkHref)
{
}
bool WMSLayerAttribution::IsValid() const
{
	return !xlinkHref.empty();
}

WMSLayerAuthorityUrl::WMSLayerAuthorityUrl(const string& name, const string& xlinkHref) :name(name), xlinkHref(xlinkHref)
{
}
bool WMSLayerAuthorityUrl::IsValid() const
{
	return !xlinkHref.empty();
}

WMSLayerMetadataUrl::WMSLayerMetadataUrl(const string& format, const string& type, const string& xlinkHref): format(format), type(type), xlinkHref(xlinkHref)
{
}
bool WMSLayerMetadataUrl::IsValid() const
{
	return !type.empty() && !xlinkHref.empty();
}

WMSLayerFeatureListUrl::WMSLayerFeatureListUrl(const string& format, const string& xlinkHref) :format(format), xlinkHref(xlinkHref)
{
}
bool WMSLayerFeatureListUrl::IsValid() const
{
	return !xlinkHref.empty();
}

WMSLayerStyle::WMSLayerStyle(const string& name, const string& title, const string& abstract):name(name), title(title), abstract(abstract)
{
}
bool WMSLayerStyle::IsValid() const
{
	return !name.empty();
}

WMSLayer::WMSLayer(int orderID, const string& name, const string& title, const string& abstract) : orderID(orderID), name(name), title(title), abstract(abstract)
{
}
bool WMSLayer::IsValid() const
{
	if (orderID < 0 || title.empty() || !ex_GeographicBoundingBox.IsValid())
	{
		return false;
	}

	for (const auto& element : boundingBox)
	{
		if (!element.IsValid())
		{
			return false;
		}
	}

	for (const auto& element : authorityUrl)
	{
		if (!element.IsValid())
		{
			return false;
		}
	}

	for (const auto& element : metadataUrl)
	{
		if (!element.IsValid())
		{
			return false;
		}
	}

	for (const auto& element : featureListUrl)
	{
		if (!element.IsValid())
		{
			return false;
		}
	}

	for (const auto& element : style)
	{
		if (!element.IsValid())
		{
			return false;
		}
	}

	for (const auto& element : layer)
	{
		if (!element.IsValid())
		{
			return false;
		}
	}

	return true;
}

const WMTSTileMatrix* WMTSTileMatrixSet::GetTileMatrix(const string& identifier) const
{
	for (const auto& tileMatrix : tileMatrices)
	{
		if (tileMatrix.second.identifier.empty())
		{
			continue;
		}

		if (tileMatrix.second.identifier == identifier)
		{
			return &tileMatrix.second;
		}

		string levelIDString = tileMatrix.second.identifier;
		const size_t lastColonPos = levelIDString.find_last_of(':');
		if (lastColonPos != string::npos && lastColonPos < levelIDString.size() - 1) // 例如"EPSG:4326:1", 得到"1"
		{
			levelIDString = levelIDString.substr(lastColonPos + 1);
		}
		if (levelIDString == identifier || levelIDString == "0" + identifier) // 例如"01"
		{
			return &tileMatrix.second;
		}
	}
	return nullptr;
}

bool WMTSTileLayer::TileMatrixSetLink::TileMatrixLimits::IsValid() const
{
	return !tileMatrix.empty() && minTileRow >= 0 && maxTileRow >= 0 && minTileCol >= 0 && maxTileCol >= 0 && minTileRow <= maxTileRow && minTileCol <= maxTileCol;
}

bool WMTSTileLayer::TileMatrixSetLink::TileMatrixLimits::IsValid(int level) const
{
	return tileMatrix == to_string(level) && minTileRow >= 0 && maxTileRow >= 0 && minTileCol >= 0 && maxTileCol >= 0 && minTileRow <= maxTileRow && minTileCol <= maxTileCol;
}

LayerTree::LayerTree() :rootOrderID(-1)
{
	subLayers.clear();
}

LayerTree::LayerTree(int rootOrderID) : rootOrderID(rootOrderID)
{
	subLayers.clear();
}


vector<LayerTree> LayerTree::GenerateLayerTree(const unordered_map<int, int>& layerParents)
{
	unordered_map<int, unique_ptr<LayerTree>> nodeMap;
	unordered_set<int> children;
	int maxNodeID = -1;

	for (const auto& [child, parent] : layerParents)
	{
		if (!nodeMap.count(child))
		{
			nodeMap[child] = make_unique<LayerTree>(child);
		}
		if (!nodeMap.count(parent))
		{
			nodeMap[parent] = make_unique<LayerTree>(parent);
		}
		maxNodeID = max(child, maxNodeID);

		nodeMap[parent]->subLayers.push_back(*nodeMap[child]);
		children.insert(child);
	}

	vector<LayerTree> roots;
	for (const auto& [id, node] : nodeMap)
	{
		if (!children.count(id))
		{
			roots.push_back(*node);
		}
	}

	for (int id = 0; id <= maxNodeID; id++)
	{
		if (!layerParents.count(id) &&
			!any_of(roots.begin(), roots.end(), [id](const LayerTree& node) { return node.rootOrderID == id; }))
		{
			roots.push_back(LayerTree(id));
		}
	}

	for (auto& root : roots)
	{
		root.SortRecursive();
	}

	sort(roots.begin(), roots.end(), [](const LayerTree& a, const LayerTree& b) {
		return a.rootOrderID < b.rootOrderID;
		});

	return roots;
}

void LayerTree::SortRecursive()
{
	sort(subLayers.begin(), subLayers.end(), [](const LayerTree& a, const LayerTree& b) {
		return a.rootOrderID < b.rootOrderID;
		});

	for (auto& sub : subLayers)
	{
		sub.SortRecursive();
	}
}


