#include <assert.h>
#include <stdio.h>
#include <memory>

#include "../ClipperUtils.hpp"
#include "../Geometry.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "../PrintConfig.hpp"
#include "../Surface.hpp"

#include "FillBase.hpp"

namespace Slic3r {

struct SurfaceGroupAttrib
{
    SurfaceGroupAttrib() : is_solid(false), flow_width(0.f), pattern(-1) {}
    bool operator==(const SurfaceGroupAttrib &other) const
        { return is_solid == other.is_solid && flow_width == other.flow_width && pattern == other.pattern; }
    bool    is_solid;
    float   flow_width;
    // pattern is of type InfillPattern, -1 for an unset pattern.
    int     pattern;
};

// Generate infills for Slic3r::Layer::Region.
// The Slic3r::Layer::Region at this point of time may contain
// surfaces of various types (internal/bridge/top/bottom/solid).
// The infills are generated on the groups of surfaces with a compatible type. 
// Returns an array of Slic3r::ExtrusionPath::Collection objects containing the infills generaed now
// and the thin fills generated by generate_perimeters().
void make_fill(LayerRegion &layerm, ExtrusionEntityCollection &out)
{    
//    Slic3r::debugf "Filling layer %d:\n", $layerm->layer->id;
    
    double  fill_density            = layerm.region()->config.fill_density;
    Flow    infill_flow             = layerm.flow(frInfill);
    Flow    solid_infill_flow       = layerm.flow(frSolidInfill);
    Flow    top_solid_infill_flow   = layerm.flow(frTopSolidInfill);
    const float perimeter_spacing = layerm.flow(frPerimeter).spacing();

    Surfaces surfaces;
    
    // merge adjacent surfaces
    // in case of bridge surfaces, the ones with defined angle will be attached to the ones
    // without any angle (shouldn't this logic be moved to process_external_surfaces()?)
    {
        Polygons polygons_bridged;
        polygons_bridged.reserve(layerm.fill_surfaces.surfaces.size());
        for (Surfaces::iterator it = layerm.fill_surfaces.surfaces.begin(); it != layerm.fill_surfaces.surfaces.end(); ++ it)
            if (it->bridge_angle >= 0)
                polygons_append(polygons_bridged, *it);
        
        // group surfaces by distinct properties (equal surface_type, thickness, thickness_layers, bridge_angle)
        // group is of type Slic3r::SurfaceCollection
        //FIXME: Use some smart heuristics to merge similar surfaces to eliminate tiny regions.
        std::vector<SurfacesPtr> groups;
        layerm.fill_surfaces.group(&groups);

        //if internal infill can be dense, place it on his own group
        if (layerm.region()->config.infill_dense.getBool() && layerm.region()->config.fill_density<40) {
            SurfacesPtr *denseGroup = NULL;
            const uint32_t nbGroups = groups.size();
            for (uint32_t num_group = 0; num_group < nbGroups; ++num_group) {
                for (uint32_t num_srf = 0; num_srf < groups[num_group].size(); ++num_srf) {
                    Surface *srf = groups[num_group][num_srf];
                    //if solid, wrong group
                    if (srf->is_solid()) break;
                    //find surface that can be used as dense infill
                    if (srf->maxNbSolidLayersOnTop <= 1
                        && srf->maxNbSolidLayersOnTop > 0) {
                        //remove from the group
                        groups[num_group].erase(groups[num_group].begin() + num_srf);
                        --num_srf;
                        //add to the "dense" group
                        if (denseGroup == NULL) {
                            groups.resize(groups.size() + 1);
                            denseGroup = &groups.back();
                        }
                        denseGroup->push_back(srf);
                    }
                }
            }
        }
        
        // merge compatible groups (we can generate continuous infill for them)
        {
            // cache flow widths and patterns used for all solid groups
            // (we'll use them for comparing compatible groups)
            std::vector<SurfaceGroupAttrib> group_attrib(groups.size());
            for (size_t i = 0; i < groups.size(); ++ i) {
                // we can only merge solid non-bridge non-overextruded surfaces, so discard
                // non-solid surfaces
                const Surface &surface = *groups[i].front();
                if (surface.is_solid() && (!surface.is_bridge() || layerm.layer()->id() == 0) && !surface.is_overBridge()) {
                    group_attrib[i].is_solid = true;
                    group_attrib[i].flow_width = (surface.is_top()) ? top_solid_infill_flow.width : solid_infill_flow.width;
                    group_attrib[i].pattern = (surface.is_top() && surface.is_external()) ? layerm.region()->config.top_fill_pattern.value : ipRectilinear;
                    group_attrib[i].pattern = (surface.is_bottom() && surface.is_external()) ? layerm.region()->config.bottom_fill_pattern.value : ipRectilinear;
                }
            }
            // Loop through solid groups, find compatible groups and append them to this one.
            for (size_t i = 0; i < groups.size(); ++ i) {
                if (! group_attrib[i].is_solid)
                    continue;
                for (size_t j = i + 1; j < groups.size();) {
                    if (group_attrib[i] == group_attrib[j]) {
                        // groups are compatible, merge them
                        groups[i].insert(groups[i].end(), groups[j].begin(), groups[j].end());
                        groups.erase(groups.begin() + j);
                        group_attrib.erase(group_attrib.begin() + j);
                    } else
                         ++ j;
                }
            }
        }
        
        // Give priority to bridges. Process the bridges in the first round, the rest of the surfaces in the 2nd round.
        for (size_t round = 0; round < 2; ++ round) {
            for (std::vector<SurfacesPtr>::iterator it_group = groups.begin(); it_group != groups.end(); ++it_group) {
                const SurfacesPtr &group = *it_group;
                bool is_bridge = group.front()->bridge_angle >= 0;
                if (is_bridge != (round == 0))
                    continue;
                // Make a union of polygons defining the infiill regions of a group, use a safety offset.
                Polygons union_p = union_(to_polygons(*it_group), true);
                // Subtract surfaces having a defined bridge_angle from any other, use a safety offset.
                if (!polygons_bridged.empty() && !is_bridge)
                    union_p = diff(union_p, polygons_bridged, true);
                // subtract any other surface already processed
                //FIXME Vojtech: Because the bridge surfaces came first, they are subtracted twice!
                // Using group.front() as a template.
                surfaces_append(surfaces, diff_ex(union_p, to_polygons(surfaces), true), *group.front());
                
            }
        }
    }
    
    // we need to detect any narrow surfaces that might collapse
    // when adding spacing below
    // such narrow surfaces are often generated in sloping walls
    // by bridge_over_infill() and combine_infill() as a result of the
    // subtraction of the combinable area from the layer infill area,
    // which leaves small areas near the perimeters
    // we are going to grow such regions by overlapping them with the void (if any)
    // TODO: detect and investigate whether there could be narrow regions without
    // any void neighbors
    {
        coord_t distance_between_surfaces = std::max(
            std::max(infill_flow.scaled_spacing(), solid_infill_flow.scaled_spacing()),
            top_solid_infill_flow.scaled_spacing());
        Polygons surfaces_polygons = to_polygons(surfaces);
        Polygons collapsed = diff(
            surfaces_polygons,
            offset2(surfaces_polygons, -distance_between_surfaces/2, +distance_between_surfaces/2),
            true);
        Polygons to_subtract;
        to_subtract.reserve(collapsed.size() + number_polygons(surfaces));
        for (Surfaces::const_iterator it_surface = surfaces.begin(); it_surface != surfaces.end(); ++ it_surface)
            if (it_surface->surface_type == stInternalVoid)
                polygons_append(to_subtract, *it_surface);
        polygons_append(to_subtract, collapsed);
        surfaces_append(
            surfaces,
            intersection_ex(
                offset(collapsed, distance_between_surfaces),
                to_subtract,
                true),
            stInternalSolid);
    }

    if (0) {
//        require "Slic3r/SVG.pm";
//        Slic3r::SVG::output("fill_" . $layerm->print_z . ".svg",
//            expolygons      => [ map $_->expolygon, grep !$_->is_solid, @surfaces ],
//            red_expolygons  => [ map $_->expolygon, grep  $_->is_solid, @surfaces ],
//        );
    }
    for (const Surface &surface : surfaces) {
        if (surface.surface_type == stInternalVoid)
            continue;
        InfillPattern  fill_pattern = layerm.region()->config.fill_pattern.value;
        double         density      = fill_density;
        FlowRole role = (surface.is_top()) ? frTopSolidInfill :
            (surface.is_solid() ? frSolidInfill : frInfill);
        bool is_bridge = layerm.layer()->id() > 0 && surface.is_bridge();
        bool is_denser = false;

        if (surface.is_solid()) {
            density = 100.;
            fill_pattern = (surface.is_external() && ! is_bridge) ? 
                (surface.is_top() ? layerm.region()->config.top_fill_pattern.value : layerm.region()->config.bottom_fill_pattern.value) :
                ipRectilinear;
        } else {
            if (layerm.region()->config.infill_dense.getBool()
                && layerm.region()->config.fill_density<40
                && surface.maxNbSolidLayersOnTop <= 1
                && surface.maxNbSolidLayersOnTop > 0) {
                density = 42;
                is_denser = true;
                fill_pattern = ipRectiWithPerimeter;
            }
            if (density <= 0)
                continue;
        }
        
        // get filler object
        std::unique_ptr<Fill> f = std::unique_ptr<Fill>(Fill::new_from_type(fill_pattern));
        f->set_bounding_box(layerm.layer()->object()->bounding_box());
        
        // calculate the actual flow we'll be using for this infill
        coordf_t h = (surface.thickness == -1) ? layerm.layer()->height : surface.thickness;
        Flow flow = layerm.region()->flow(
            role,
            h,
            is_bridge || f->use_bridge_flow(),  // bridge flow?
            layerm.layer()->id() == 0,          // first layer?
            -1,                                 // auto width
            *layerm.layer()->object()
        );
        
        // calculate flow spacing for infill pattern generation
        bool using_internal_flow = false;
        if (! surface.is_solid() && ! is_bridge) {
            // it's internal infill, so we can calculate a generic flow spacing 
            // for all layers, for avoiding the ugly effect of
            // misaligned infill on first layer because of different extrusion width and
            // layer height
            Flow internal_flow = layerm.region()->flow(
                frInfill,
                layerm.layer()->object()->config.layer_height.value,  // TODO: handle infill_every_layers?
                false,  // no bridge
                false,  // no first layer
                -1,     // auto width
                *layerm.layer()->object()
            );
            f->spacing = internal_flow.spacing();
            using_internal_flow = true;
        } else {
            f->spacing = flow.spacing();
        }

        double link_max_length = 0.;
        if (! is_bridge) {
#if 0
            link_max_length = layerm.region()->config.get_abs_value(surface.is_external() ? "external_fill_link_max_length" : "fill_link_max_length", flow.spacing());
//            printf("flow spacing: %f,  is_external: %d, link_max_length: %lf\n", flow.spacing(), int(surface.is_external()), link_max_length);
#else
            if (density > 80.) // 80%
                link_max_length = 3. * f->spacing;
#endif
        }

        f->layer_id = layerm.layer()->id();
        f->z = layerm.layer()->print_z;
        if (is_denser)f->angle = 0;
        else f->angle = float(Geometry::deg2rad(layerm.region()->config.fill_angle.value));
        // Maximum length of the perimeter segment linking two infill lines.
        f->link_max_length = scale_(link_max_length);
        // Used by the concentric infill pattern to clip the loops to create extrusion paths.
        f->loop_clipping = scale_(flow.nozzle_diameter) * LOOP_CLIPPING_LENGTH_OVER_NOZZLE_DIAMETER;
        //give the overlap size to let the infill do his overlap
        //add overlap if at least one perimeter
        if (layerm.region()->config.perimeters.getInt() > 0) {
            f->overlap = layerm.region()->config.get_abs_value("infill_overlap", (perimeter_spacing + (f->spacing)) / 2);
            if (f->overlap!=0) {
                f->no_overlap_expolygons = intersection_ex(layerm.fill_no_overlap_expolygons, ExPolygons() = { surface.expolygon });
            } else {
                f->no_overlap_expolygons.push_back(surface.expolygon);
            }
        } else {
            f->overlap = 0;
            f->no_overlap_expolygons.push_back(surface.expolygon);
        }
        
        // apply half spacing using this flow's own spacing and generate infill
        FillParams params;
        params.density = 0.01 * density;
        params.dont_adjust = false;
        params.fill_exactly = layerm.region()->config.enforce_full_fill_volume.getBool();
        params.dont_connect = layerm.region()->config.infill_not_connected.getBool();

        // calculate actual flow from spacing (which might have been adjusted by the infill
        // pattern generator)
        if (using_internal_flow) {
            // if we used the internal flow we're not doing a solid infill
            // so we can safely ignore the slight variation that might have
            // been applied to $f->flow_spacing
        } else {
            flow = Flow::new_from_spacing(f->spacing, flow.nozzle_diameter, h, is_bridge || f->use_bridge_flow());
        }
        
        float flow_percent = 1;
        if(surface.is_overBridge()){
            params.density = layerm.region()->config.over_bridge_flow_ratio;
            //params.flow_mult = layerm.region()->config.over_bridge_flow_ratio;
        }
        
        f->fill_surface_extrusion(&surface, params, flow, erNone, out.entities);
    }

    // add thin fill regions
    // thin_fills are of C++ Slic3r::ExtrusionEntityCollection, perl type Slic3r::ExtrusionPath::Collection
    // Unpacks the collection, creates multiple collections per path.
    // The path type could be ExtrusionPath, ExtrusionLoop or ExtrusionEntityCollection.
    // Why the paths are unpacked?
    for (const ExtrusionEntity *thin_fill : layerm.thin_fills.entities) {
        ExtrusionEntityCollection &collection = *(new ExtrusionEntityCollection());
        out.entities.push_back(&collection);
        collection.entities.push_back(thin_fill->clone());
    }
}

} // namespace Slic3r
