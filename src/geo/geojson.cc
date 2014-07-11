// Copyright 2010-2014 RethinkDB, all rights reserved.
#include "geo/geojson.hpp"

#include "errors.hpp"
#include <boost/ptr_container/ptr_vector.hpp>

#include <string>
#include <vector>

#include "containers/scoped.hpp"
#include "containers/wire_string.hpp"
#include "geo/exceptions.hpp"
#include "geo/geo_visitor.hpp"
#include "geo/s2/s1angle.h"
#include "geo/s2/s2.h"
#include "geo/s2/s2latlng.h"
#include "geo/s2/s2loop.h"
#include "geo/s2/s2polygon.h"
#include "geo/s2/s2polygonbuilder.h"
#include "geo/s2/s2polyline.h"
#include "rdb_protocol/datum.hpp"

#include "debug.hpp" // TODO!

using ql::datum_t;
using ql::datum_ptr_t;

counted_t<const ql::datum_t> construct_geo_point(const lat_lon_point_t &point) {
    datum_ptr_t result(datum_t::R_OBJECT);
    bool dup;
    dup = result.add("$reql_type$", make_counted<datum_t>("geometry"));
    r_sanity_check(!dup);
    dup = result.add("type", make_counted<datum_t>("Point"));
    r_sanity_check(!dup);

    std::vector<counted_t<const datum_t> > coordinates;
    coordinates.reserve(2);
    // lat, long -> long, lat
    coordinates.push_back(make_counted<datum_t>(point.second));
    coordinates.push_back(make_counted<datum_t>(point.first));
    dup = result.add("coordinates", make_counted<datum_t>(std::move(coordinates)));
    r_sanity_check(!dup);

    return result.to_counted();
}

std::vector<counted_t<const datum_t> > construct_line_coordinates(
        const lat_lon_line_t &line) {
    std::vector<counted_t<const datum_t> > coordinates;
    coordinates.reserve(line.size());
    for (size_t i = 0; i < line.size(); ++i) {
        std::vector<counted_t<const datum_t> > point;
        point.reserve(2);
        // lat, long -> long, lat
        point.push_back(make_counted<datum_t>(line[i].second));
        point.push_back(make_counted<datum_t>(line[i].first));
        coordinates.push_back(make_counted<datum_t>(std::move(point)));
    }
    return coordinates;
}

counted_t<const ql::datum_t> construct_geo_line(const lat_lon_line_t &line) {
    datum_ptr_t result(datum_t::R_OBJECT);
    bool dup;
    dup = result.add("$reql_type$", make_counted<datum_t>("geometry"));
    r_sanity_check(!dup);
    dup = result.add("type", make_counted<datum_t>("LineString"));
    r_sanity_check(!dup);

    dup = result.add("coordinates",
                      make_counted<datum_t>(std::move(construct_line_coordinates(line))));
    r_sanity_check(!dup);

    return result.to_counted();
}

counted_t<const ql::datum_t> construct_geo_polygon(const lat_lon_line_t &shell) {
    datum_ptr_t result(datum_t::R_OBJECT);
    bool dup;
    dup = result.add("$reql_type$", make_counted<datum_t>("geometry"));
    r_sanity_check(!dup);
    dup = result.add("type", make_counted<datum_t>("Polygon"));
    r_sanity_check(!dup);

    std::vector<counted_t<const datum_t> > shell_coordinates =
        construct_line_coordinates(shell);
    // Close the line
    if (shell.size() >= 1 && shell[0] != shell[shell.size()-1]) {
        rassert(!shell_coordinates.empty());
        shell_coordinates.push_back(shell_coordinates[0]);
    }
    std::vector<counted_t<const datum_t> > coordinates;
    coordinates.push_back(make_counted<datum_t>(std::move(shell_coordinates)));
    dup = result.add("coordinates", make_counted<datum_t>(std::move(coordinates)));
    r_sanity_check(!dup);

    return result.to_counted();
}

// Parses a GeoJSON "Position" array
lat_lon_point_t position_to_lat_lon_point(const counted_t<const datum_t> &position) {
    // This assumes the default spherical GeoJSON coordinate reference system,
    // with latitude and longitude given in degrees.

    const std::vector<counted_t<const datum_t> > arr = position->as_array();
    if (arr.size() < 2) {
        throw geo_exception_t(
            "Too few coordinates. Need at least longitude and latitude.");
    }
    if (arr.size() > 3) {
        throw geo_exception_t(
            strprintf("Too many coordinates. GeoJSON position should have no more than "
                      "three coordinates, but got %zu", arr.size()));
    }
    if (arr.size() == 3) {
        // TODO! Error?
        debugf("Ignoring altitude in GeoJSON position.");
    }

    // GeoJSON positions are in order longitude, latitude, altitude
    double longitude, latitude;
    longitude = arr[0]->as_num();
    latitude = arr[1]->as_num();

    // Range checks.
    // (S2 crashes badly if those are violated)
    if(longitude < 0.0 || longitude > 180.0) {
        throw geo_range_exception_t(
            strprintf("Longitude must be between 0 and 180. Got %f.", longitude));
    }
    if (longitude == 180.0) {
        longitude = 0.0;
    }
    if(latitude < -90.0 || latitude > 90.0) {
        throw geo_range_exception_t(
            strprintf("Latitude must be between -90 and 90. Got %f.", latitude));
    }
    if (latitude == 90.0) {
        latitude = -90.0;
    }

    return lat_lon_point_t(latitude, longitude);
}

lat_lon_point_t extract_lat_lon_point(const counted_t<const datum_t> &geojson) {;
    if (geojson->get("type")->as_str().to_std() != "Point") {
        throw geo_exception_t(
            strprintf("Expected Point, but got %s", geojson->get("type")->as_str().c_str()));
    }

    const counted_t<const datum_t> &coordinates = geojson->get("coordinates");

    return position_to_lat_lon_point(coordinates);
}

// Parses a GeoJSON "Position" array
S2Point position_to_s2point(const counted_t<const datum_t> &position) {
    lat_lon_point_t p = position_to_lat_lon_point(position);

    S2LatLng lat_lng(S1Angle::Degrees(p.first), S1Angle::Degrees(p.second));
    return lat_lng.ToPoint();
}

scoped_ptr_t<S2Point> coordinates_to_s2point(const counted_t<const datum_t> &coords) {
    /* From the specs:
        "For type "Point", the "coordinates" member must be a single position."
    */
    S2Point pos = position_to_s2point(coords);
    return scoped_ptr_t<S2Point>(new S2Point(pos));
}

scoped_ptr_t<S2Polyline> coordinates_to_s2polyline(const counted_t<const datum_t> &coords) {
    /* From the specs:
        "For type "LineString", the "coordinates" member must be an array of two
         or more positions."
    */
    // TODO! We must catch the case where points are antipodal or identical.
    // Currently S2 crashes.
    const std::vector<counted_t<const datum_t> > arr = coords->as_array();
    if (arr.size() < 2) {
        throw geo_exception_t(
            "GeoJSON LineString must have at least two positions.");
    }
    std::vector<S2Point> points;
    points.reserve(arr.size());
    for (size_t i = 0; i < arr.size(); ++i) {
        points.push_back(position_to_s2point(arr[i]));
    }
    return scoped_ptr_t<S2Polyline>(new S2Polyline(points));
}

scoped_ptr_t<S2Loop> coordinates_to_s2loop(const counted_t<const datum_t> &coords) {
    // Like a LineString, but must be connected
    const std::vector<counted_t<const datum_t> > arr = coords->as_array();
    if (arr.size() < 4) {
        throw geo_exception_t(
            "GeoJSON LinearRing must have at least four positions.");
    }
    std::vector<S2Point> points;
    points.reserve(arr.size());
    for (size_t i = 0; i < arr.size(); ++i) {
        points.push_back(position_to_s2point(arr[i]));
    }
    if (points[0] != points[points.size()-1]) {
        throw geo_exception_t(
            "First and last vertex of GeoJSON LinearRing must be identical.");
    }
    // Drop the last point. S2Loop closes the loop implicitly.
    points.pop_back();
    scoped_ptr_t<S2Loop> result(new S2Loop(points));
    // Normalize the loop
    result->Normalize();
    return std::move(result);
}

scoped_ptr_t<S2Polygon> coordinates_to_s2polygon(const counted_t<const datum_t> &coords) {
    /* From the specs:
        "For type "Polygon", the "coordinates" member must be an array of LinearRing
         coordinate arrays. For Polygons with multiple rings, the first must be the
         exterior ring and any others must be interior rings or holes."
    */
    const std::vector<counted_t<const datum_t> > loops_arr = coords->as_array();
    boost::ptr_vector<S2Loop> loops;
    loops.reserve(loops_arr.size());
    for (size_t i = 0; i < loops_arr.size(); ++i) {
        scoped_ptr_t<S2Loop> loop = coordinates_to_s2loop(loops_arr[i]);
        // TODO! Does PolygonBuilder already perform this validation?
        if (!loop->IsValid()) {
            throw geo_exception_t(
                "Invalid LinearRing (are there duplicate vertices? is it "
                "self-intersecting?)");
        }
        // Put the loop into the ptr_vector to avoid its destruction while
        // we are still building the polygon
        loops.push_back(loop.release());
    }

    // The first loop must be the outer shell, all other loops must be holes.
    // Invert the inner loops.
    for (size_t i = 1; i < loops.size(); ++i) {
        loops[i].Invert();
    }

    // We use S2PolygonBuilder to automatically clean up identical edges and such
    S2PolygonBuilderOptions builder_opts = S2PolygonBuilderOptions::DIRECTED_XOR();
    // We want validation... for now
    // TODO! Don't validate more often than necessary. It's probably expensive.
    builder_opts.set_validate(true);
    S2PolygonBuilder builder(builder_opts);
    for (size_t i = 0; i < loops.size(); ++i) {
        builder.AddLoop(&loops[i]);
    }

    scoped_ptr_t<S2Polygon> result(new S2Polygon());
    S2PolygonBuilder::EdgeList unused_edges;
    builder.AssemblePolygon(result.get(), &unused_edges);
    if (!unused_edges.empty()) {
        throw geo_exception_t(
            "Some edges in GeoJSON polygon could not be used.");
    }

    return std::move(result);
}

scoped_ptr_t<S2Point> to_s2point(const counted_t<const ql::datum_t> &geojson) {
    // TODO! Ensure that geojson has either no "crs" member, or it is null.
    // Fail otherwise.
    const std::string type = geojson->get("type")->as_str().to_std();
    counted_t<const datum_t> coordinates = geojson->get("coordinates");
    if (type != "Point") {
        throw geo_exception_t("Encountered wrong type in to_s2point");
    }
    return coordinates_to_s2point(coordinates);
}

scoped_ptr_t<S2Polyline> to_s2polyline(const counted_t<const ql::datum_t> &geojson) {
    // TODO! Ensure that geojson has either no "crs" member, or it is null.
    // Fail otherwise.
    const std::string type = geojson->get("type")->as_str().to_std();
    counted_t<const datum_t> coordinates = geojson->get("coordinates");
    if (type != "LineString") {
        throw geo_exception_t("Encountered wrong type in to_s2polyline");
    }
    return coordinates_to_s2polyline(coordinates);
}

scoped_ptr_t<S2Polygon> to_s2polygon(const counted_t<const ql::datum_t> &geojson) {
    // TODO! Ensure that geojson has either no "crs" member, or it is null.
    // Fail otherwise.
    const std::string type = geojson->get("type")->as_str().to_std();
    counted_t<const datum_t> coordinates = geojson->get("coordinates");
    if (type != "Polygon") {
        throw geo_exception_t("Encountered wrong type in to_s2polygon");
    }
    return coordinates_to_s2polygon(coordinates);
}

// TODO! What does it throw?
void visit_geojson(s2_geo_visitor_t *visitor, const counted_t<const datum_t> &geojson) {
    // TODO! Ensure that geojson has either no "crs" member, or it is null.
    // Fail otherwise.

    const std::string type = geojson->get("type")->as_str().to_std();
    counted_t<const datum_t> coordinates = geojson->get("coordinates");

    if (type == "Point") {
        scoped_ptr_t<S2Point> pt = coordinates_to_s2point(coordinates);
        rassert(pt.has());
        visitor->on_point(*pt);
    } else if (type == "LineString") {
        scoped_ptr_t<S2Polyline> l = coordinates_to_s2polyline(coordinates);
        rassert(l.has());
        visitor->on_line(*l);
    } else if (type == "Polygon") {
        scoped_ptr_t<S2Polygon> poly = coordinates_to_s2polygon(coordinates);
        rassert(poly.has());
        visitor->on_polygon(*poly);
    } else {
        bool valid_geojson =
            type == "MultiPoint"
            || type == "MultiLineString"
            || type == "MultiPolygon"
            || type == "GeometryCollection"
            || type == "Feature"
            || type == "FeatureCollection";
        if (valid_geojson) {
            throw geo_exception_t(
                strprintf("GeoJSON type %s not supported", type.c_str()));
        } else {
            throw geo_exception_t(
                strprintf("Unrecognized GeoJSON type %s", type.c_str()));
        }
    }
}

void validate_geojson(const counted_t<const ql::datum_t> &geojson) {
    // We rely on `visit_geojson()` to perform all necessary validation
    // TODO! Change that. Probably visit_geojson() should do less validation,
    //       and we should do more.
    // TODO! Check that the GeoJSON doesn't specify a reference system
    class validator_t : public s2_geo_visitor_t {
    public:
        void on_point(UNUSED const S2Point &) { }
        void on_line(UNUSED const S2Polyline &) { }
        void on_polygon(UNUSED const S2Polygon &) { }
    };
    validator_t validator;
    visit_geojson(&validator, geojson);
}

