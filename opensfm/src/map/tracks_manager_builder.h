#pragma once

#include <map/tracks_manager.h>
#include <pybind11/pybind11.h>

namespace map {

TracksManager CreateTracksManager(
    pybind11::dict features, pybind11::dict colors,
    pybind11::dict segmentations, pybind11::dict instances,
    pybind11::dict matches, int min_track_length);

}  // namespace map
