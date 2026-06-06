#include <map/tracks_manager_builder.h>

#include <map/defines.h>
#include <map/observation.h>
#include <pybind11/numpy.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace py = pybind11;

namespace {

enum class NumericType {
  DOUBLE,
  FLOAT,
  INT,
  INT64,
  UINT8,
  UINT16,
  INT16,
};

struct NumericArray {
  explicit NumericArray(py::handle value)
      : owner(py::reinterpret_borrow<py::array>(value)),
        info(owner.request()) {
    const auto& format = info.format;
    if (format == py::format_descriptor<double>::format()) {
      type = NumericType::DOUBLE;
    } else if (format == py::format_descriptor<float>::format()) {
      type = NumericType::FLOAT;
    } else if (format == py::format_descriptor<int>::format()) {
      type = NumericType::INT;
    } else if (format == py::format_descriptor<long long>::format()) {
      type = NumericType::INT64;
    } else if (format == py::format_descriptor<unsigned char>::format()) {
      type = NumericType::UINT8;
    } else if (format == py::format_descriptor<unsigned short>::format()) {
      type = NumericType::UINT16;
    } else if (format == py::format_descriptor<short>::format()) {
      type = NumericType::INT16;
    } else {
      throw std::runtime_error("Unsupported NumPy dtype in track construction");
    }
  }

  py::array owner;
  py::buffer_info info;
  NumericType type;
};

struct ImageFeatureData {
  ImageFeatureData(py::handle points_value, py::handle colors_value)
      : points(points_value), colors(colors_value) {}

  NumericArray points;
  NumericArray colors;
  std::unique_ptr<NumericArray> segmentations;
  std::unique_ptr<NumericArray> instances;
};

double ReadNumericValue(const NumericArray& array, py::ssize_t row,
                        py::ssize_t column = 0) {
  const auto& info = array.info;
  if (info.ndim < 1 || info.ndim > 2) {
    throw std::runtime_error("Expected a one or two dimensional NumPy array");
  }
  if (row < 0 || row >= info.shape[0]) {
    throw std::runtime_error("Feature index is outside the feature array");
  }
  if (info.ndim == 2 && (column < 0 || column >= info.shape[1])) {
    throw std::runtime_error("Column index is outside the feature array");
  }

  const char* data =
      static_cast<const char*>(info.ptr) + row * info.strides[0];
  if (info.ndim == 2) {
    data += column * info.strides[1];
  }

  switch (array.type) {
    case NumericType::DOUBLE:
      return *reinterpret_cast<const double*>(data);
    case NumericType::FLOAT:
      return *reinterpret_cast<const float*>(data);
    case NumericType::INT:
      return *reinterpret_cast<const int*>(data);
    case NumericType::INT64:
      return *reinterpret_cast<const long long*>(data);
    case NumericType::UINT8:
      return *reinterpret_cast<const unsigned char*>(data);
    case NumericType::UINT16:
      return *reinterpret_cast<const unsigned short*>(data);
    case NumericType::INT16:
      return *reinterpret_cast<const short*>(data);
  }
  throw std::runtime_error("Invalid NumPy dtype in track construction");
}

struct TrackNode {
  int image;
  int feature;
  int parent;
  int rank{0};
};

int FindRoot(std::vector<TrackNode>* nodes, int node) {
  int root = node;
  while ((*nodes)[root].parent != root) {
    root = (*nodes)[root].parent;
  }
  while ((*nodes)[node].parent != node) {
    const int parent = (*nodes)[node].parent;
    (*nodes)[node].parent = root;
    node = parent;
  }
  return root;
}

void UnionNodes(std::vector<TrackNode>* nodes, int first, int second) {
  int first_root = FindRoot(nodes, first);
  int second_root = FindRoot(nodes, second);
  if (first_root == second_root) {
    return;
  }
  if ((*nodes)[first_root].rank < (*nodes)[second_root].rank) {
    std::swap(first_root, second_root);
  }
  (*nodes)[second_root].parent = first_root;
  if ((*nodes)[first_root].rank == (*nodes)[second_root].rank) {
    ++(*nodes)[first_root].rank;
  }
}

}  // namespace

namespace map {

TracksManager CreateTracksManager(
    py::dict features, py::dict colors, py::dict segmentations,
    py::dict instances, py::dict matches, int min_track_length) {
  using FeatureKey = std::pair<int, int>;

  std::unordered_map<std::string, std::unique_ptr<ImageFeatureData>> image_data;
  image_data.reserve(features.size());
  for (const auto& item : features) {
    const auto image = py::cast<std::string>(item.first);
    if (!colors.contains(item.first)) {
      continue;
    }
    auto data = std::unique_ptr<ImageFeatureData>(
        new ImageFeatureData(item.second, colors[item.first]));
    if (segmentations.contains(item.first)) {
      data->segmentations.reset(new NumericArray(segmentations[item.first]));
    }
    if (instances.contains(item.first)) {
      data->instances.reset(new NumericArray(instances[item.first]));
    }
    image_data.emplace(image, std::move(data));
  }

  std::vector<TrackNode> nodes;
  std::vector<std::string> image_names;
  std::unordered_map<std::string, int> image_indices;
  std::unordered_map<FeatureKey, int, HashPair> node_indices;
  std::vector<std::pair<int, int>> edges;

  auto get_image_index = [&image_names, &image_indices](
                             const std::string& image) {
    const auto found = image_indices.find(image);
    if (found != image_indices.end()) {
      return found->second;
    }
    const int index = static_cast<int>(image_names.size());
    image_names.push_back(image);
    image_indices.emplace(image, index);
    return index;
  };

  auto get_node = [&nodes, &node_indices,
                   &get_image_index](const std::string& image, int feature) {
    const int image_index = get_image_index(image);
    const FeatureKey key(image_index, feature);
    const auto found = node_indices.find(key);
    if (found != node_indices.end()) {
      return found->second;
    }
    const int index = static_cast<int>(nodes.size());
    nodes.push_back({image_index, feature, index, 0});
    node_indices.emplace(key, index);
    return index;
  };

  for (const auto& item : matches) {
    const auto pair = py::reinterpret_borrow<py::tuple>(item.first);
    if (pair.size() != 2) {
      throw std::runtime_error("Match dictionary keys must be image pairs");
    }
    const auto image1 = py::cast<std::string>(pair[0]);
    const auto image2 = py::cast<std::string>(pair[1]);
    auto pair_matches =
        py::array_t<int, py::array::c_style | py::array::forcecast>::ensure(
            item.second);
    if (!pair_matches) {
      throw std::runtime_error("Pair matches must be a NumPy array");
    }
    const auto info = pair_matches.request();
    if (info.ndim != 2 || info.shape[1] != 2) {
      throw std::runtime_error("Pair matches must have shape (N, 2)");
    }
    const int* match_data = static_cast<const int*>(info.ptr);
    edges.reserve(edges.size() + static_cast<size_t>(info.shape[0]));
    for (py::ssize_t i = 0; i < info.shape[0]; ++i) {
      edges.emplace_back(get_node(image1, match_data[2 * i]),
                         get_node(image2, match_data[2 * i + 1]));
    }
  }

  TracksManager tracks_manager;
  {
    py::gil_scoped_release release;
    for (const auto& edge : edges) {
      UnionNodes(&nodes, edge.first, edge.second);
    }

    std::unordered_map<int, std::vector<int>> groups;
    groups.reserve(nodes.size());
    for (int i = 0; i < static_cast<int>(nodes.size()); ++i) {
      groups[FindRoot(&nodes, i)].push_back(i);
    }

    std::vector<std::vector<int>> tracks;
    tracks.reserve(groups.size());
    for (auto& group : groups) {
      auto& track = group.second;
      if (static_cast<int>(track.size()) < min_track_length) {
        continue;
      }
      std::unordered_set<int> images;
      bool valid = true;
      for (const int node_index : track) {
        if (!images.insert(nodes[node_index].image).second) {
          valid = false;
          break;
        }
      }
      if (!valid) {
        continue;
      }
      std::sort(track.begin(), track.end(), [&nodes](int first, int second) {
        const auto& a = nodes[first];
        const auto& b = nodes[second];
        return a.image < b.image ||
               (a.image == b.image && a.feature < b.feature);
      });
      tracks.emplace_back(std::move(track));
    }
    std::sort(tracks.begin(), tracks.end(), [&nodes](const auto& first,
                                                     const auto& second) {
      const auto& a = nodes[first.front()];
      const auto& b = nodes[second.front()];
      return a.image < b.image ||
             (a.image == b.image && a.feature < b.feature);
    });

    int track_id = 0;
    for (const auto& track : tracks) {
      const auto track_name = std::to_string(track_id++);
      for (const int node_index : track) {
        const auto& node = nodes[node_index];
        const auto& image = image_names[node.image];
        const auto data_it = image_data.find(image);
        if (data_it == image_data.end()) {
          continue;
        }
        const auto& data = *data_it->second;
        const int segmentation =
            data.segmentations
                ? static_cast<int>(
                      ReadNumericValue(*data.segmentations, node.feature))
                : Observation::NO_SEMANTIC_VALUE;
        const int instance =
            data.instances
                ? static_cast<int>(
                      ReadNumericValue(*data.instances, node.feature))
                : Observation::NO_SEMANTIC_VALUE;
        const Observation observation(
            ReadNumericValue(data.points, node.feature, 0),
            ReadNumericValue(data.points, node.feature, 1),
            ReadNumericValue(data.points, node.feature, 2),
            static_cast<int>(ReadNumericValue(data.colors, node.feature, 0)),
            static_cast<int>(ReadNumericValue(data.colors, node.feature, 1)),
            static_cast<int>(ReadNumericValue(data.colors, node.feature, 2)),
            node.feature, segmentation, instance);
        tracks_manager.AddObservation(image, track_name, observation);
      }
    }
  }
  return tracks_manager;
}

}  // namespace map
