/*
 * atlaas.cpp
 *
 * Atlas at LAAS
 *
 * author:  Pierrick Koch <pierrick.koch@laas.fr>
 * created: 2013-10-08
 * license: BSD
 */
#include <cassert>
#include <stdexcept>        // for out_of_range

#include <fstream>          // ofstream, tmplog
#include <algorithm>        // copy{,_backward}
#include <cmath>            // floor

#include "atlaas/atlaas.hpp"

namespace atlaas {

static std::ofstream tmplog("atlaas.log");

/**
 * Apply the transformation matrix to the point cloud (in place)
 */
void transform(points& cloud, const matrix& tr) {
    float x,y,z;
    for (auto& point : cloud) {
        x = point[0];
        y = point[1];
        z = point[2];
        point[0] = (x * tr[0]) + (y * tr[1]) + (z * tr[2])  + tr[3];
        point[1] = (x * tr[4]) + (y * tr[5]) + (z * tr[6])  + tr[7];
        point[2] = (x * tr[8]) + (y * tr[9]) + (z * tr[10]) + tr[11];
    }
}

/**
 * Merge point cloud in the internal model
 * with the sensor to world transformation,
 * and slide, save, load submodels.
 *
 * @param cloud: point cloud in the sensor frame
 * @param transformation: sensor to world transformation
 */
void atlaas::merge(points& cloud, const matrix& transformation) {
    // transform the cloud from sensor to custom frame
    transform(cloud, transformation);
    // slide map if needed. transformation[{3,7}] = {x,y}
    slide_to(transformation[3], transformation[7]);
#ifdef DYNAMIC_MERGE
    // use dynamic merge
    dynamic(cloud);
#else
    // merge the cloud in the internal data
    merge(cloud, internal);
    map_sync = false;
#endif
}

void atlaas::dynamic(const points& cloud) {
    // clear the dynamic map (zeros)
    cell_info_t zeros{}; // value-initialization w/empty initializer
    std::fill(dyninter.begin(), dyninter.end(), zeros);
    // merge the point-cloud
    merge(cloud, dyninter);
    // dyn->export8u("atlaas-dyn.jpg");
    // merge the dynamic atlaas with internal data
    merge();
}

void atlaas::sub_load(int sx, int sy) {
    std::string filepath = sub_name(current[0] + sx, current[1] + sy);
    if ( ! file_exists( filepath ) )
        return; // no file to load
    sub->init(filepath);
    auto it  = internal.begin() + sw * (sx + 1) + sh * width * (sy + 1),
         end = it + sh * width;
    for (auto sit = sub->internal.begin(); it < end; it += width, sit += sw) {
        // sub to map
        std::copy(sit, sit + sw, it);
    }
    map_sync = false;
}

void atlaas::sub_save(int sx, int sy) const {
    auto it  = internal.begin() + sw * (sx + 1) + sh * width * (sy + 1),
         end = it + sh * width;
    for (auto sit = sub->internal.begin(); it < end; it += width, sit += sw) {
        // map to sub
        std::copy(it, it + sw, sit);
    }
    sub->update();
    const auto& utm = map.point_pix2utm( sx * sw, sy * sh);
    // update map transform used for merging the pointcloud
    sub->map.set_transform(utm[0], utm[1], map.get_scale_x(), map.get_scale_y());
    sub->map.save( sub_name(current[0] + sx, current[1] + sy) );
}

/**
 * Slide, save, load submodels
 *
 * @param robx:  robot x pose in the custom frame
 * @param roby:  robot y pose in the custom frame
 */
void atlaas::slide_to(double robx, double roby) {
    const point_xy_t& pixr = map.point_custom2pix(robx, roby);
    float cx = pixr[0] / width;
    float cy = pixr[1] / height;
    // check, slide, save, load
    if ( ( cx > 0.25 ) && ( cx < 0.75 ) &&
         ( cy > 0.25 ) && ( cy < 0.75 ) )
        return; // robot is in "center" square

    int dx = (cx < 0.33) ? -1 : (cx > 0.66) ? 1 : 0; // W/E
    int dy = (cy < 0.33) ? -1 : (cy > 0.66) ? 1 : 0; // N/S
    cell_info_t zeros{}; // value-initialization w/empty initializer
    // reset state and ground infos used for dynamic merge
    std::fill(gndinter.begin(), gndinter.end(), zeros);
    std::fill(vertical.begin(), vertical.end(), false);

    if (dx == -1) {
        // save EAST 1/3 maplets [ 1,-1], [ 1, 0], [ 1, 1]
        sub_save( 1, -1);
        sub_save( 1,  0);
        sub_save( 1,  1);
        if (dy == -1) {
            // save SOUTH
            sub_save(-1,  1);
            sub_save( 0,  1);
        } else if (dy == 1) {
            // save NORTH
            sub_save(-1, -1);
            sub_save( 0, -1);
        }
        // move the map to the WEST [-1 -> 0; 0 -> 1]
        for (auto it = internal.begin(); it < internal.end(); it += width) {
            std::copy_backward(it, it + 2 * sw, it + width);
            // reset(it, it + sw);
            std::fill(it, it + sw, zeros);
        }
    } else if (dx == 1) {
        // save WEST 1/3 maplets [-1,-1], [-1, 0], [-1, 1]
        sub_save(-1, -1);
        sub_save(-1,  0);
        sub_save(-1,  1);
        if (dy == -1) {
            // save SOUTH
            sub_save( 0,  1);
            sub_save( 1,  1);
        } else if (dy == 1) {
            // save NORTH
            sub_save( 0, -1);
            sub_save( 1, -1);
        }
        // move the map to the EAST
        for (auto it = internal.begin(); it < internal.end(); it += width) {
            std::copy(it + sw, it + width, it);
            // reset(it + 2 * sw, it + width);
            std::fill(it + 2 * sw, it + width, zeros);
        }
    } else if (dy == -1) {
        // save SOUTH
        sub_save(-1,  1);
        sub_save( 0,  1);
        sub_save( 1,  1);
    } else if (dy == 1) {
        // save NORTH
        sub_save(-1, -1);
        sub_save( 0, -1);
        sub_save( 1, -1);
    }

    if (dy == -1) {
        std::copy_backward(internal.begin(), internal.end() - sh * width,
                           internal.end());
        // reset(internal.begin(), internal.begin() + sh * width);
        std::fill(internal.begin(), internal.begin() + sh * width - 1, zeros);
    } else if (dy == 1) {
        std::copy(internal.begin() + sh * width, internal.end(), internal.begin());
        // reset(internal.end() - sh * width, internal.end());
        std::fill(internal.end() - sh * width, internal.end(), zeros);
    }

    // after moving, update our current center
    current[0] += dx;
    current[1] += dy;

    // load here
    if (dx == -1) {
        // load WEST maplets
        sub_load(-1, -1);
        sub_load(-1,  0);
        sub_load(-1,  1);
        if (dy == -1) {
            // load NORTH
            sub_load( 0, -1);
            sub_load( 1, -1);
        } else if (dy == 1) {
            // load SOUTH
            sub_load( 0,  1);
            sub_load( 1,  1);
        }
    } else if (dx == 1) {
        // load EAST maplets
        sub_load( 1, -1);
        sub_load( 1,  0);
        sub_load( 1,  1);
        if (dy == -1) {
            // load NORTH
            sub_load(-1, -1);
            sub_load( 0, -1);
        } else if (dy == 1) {
            // load SOUTH
            sub_load(-1,  1);
            sub_load( 0,  1);
        }
    } else if (dy == -1) {
        // load NORTH
        sub_load(-1, -1);
        sub_load( 0, -1);
        sub_load( 1, -1);
    } else if (dy == 1) {
        // load SOUTH
        sub_load(-1,  1);
        sub_load( 0,  1);
        sub_load( 1,  1);
    }

    const auto& utm = map.point_pix2utm(sw * dx, sh * dy);
    // update map transform used for merging the pointcloud
    map.set_transform(utm[0], utm[1], map.get_scale_x(), map.get_scale_y());
    map_sync = false;
    tmplog << __func__ << " utm " << utm[0] << ", " << utm[1] << std::endl;
}

/**
 * Merge a point cloud in the internal model
 *
 * @param cloud: point cloud in the custom frame
 */
void atlaas::merge(const points& cloud, cells_info_t& inter) {
    size_t index;
    float z_mean, n_pts, new_z;
    // merge point-cloud in internal structure
    for (const auto& point : cloud) {
        index = map.index_custom(point[0], point[1]);
        if (index == std::numeric_limits<size_t>::max() )
            continue; // point is outside the map

        auto& info = inter[ index ];
        new_z = point[2];
        n_pts = info[N_POINTS];

        if (n_pts < 1) {
            info[N_POINTS] = 1;
            info[Z_MAX]  = new_z;
            info[Z_MIN]  = new_z;
            info[Z_MEAN] = new_z;
            info[VARIANCE] = 0;
        } else {
            z_mean = info[Z_MEAN];
            // increment N_POINTS
            info[N_POINTS]++;
            // update Z_MAX
            if (new_z > info[Z_MAX])
                info[Z_MAX] = new_z;
            // update Z_MIN
            if (new_z < info[Z_MIN])
                info[Z_MIN] = new_z;

            /* Incremental mean and variance updates (according to Knuth's bible,
               Vol. 2, section 4.2.2). The actual variance will later be divided
               by the number of samples plus 1. */
            info[Z_MEAN]    = (z_mean * n_pts + new_z) / info[N_POINTS];
            info[VARIANCE] += (new_z - z_mean) * (new_z - info[Z_MEAN]);
        }
    }
    map_sync = false;
}

/**
 * Compute real variance and return the mean
 */
float atlaas::variance_mean(cells_info_t& inter) {
    size_t variance_count = 0;
    float  variance_total = 0;

    for (auto& info : inter) {
        if (info[N_POINTS] > 2) {
            /* compute the real variance (according to Knuth's bible) */
            info[VARIANCE] /= info[N_POINTS] - 1;
            variance_total += info[VARIANCE];
            variance_count++;
        }
    }

    if (variance_count == 0)
        return 0;

    return variance_total / variance_count;
}

/**
 * Merge dynamic dtm
 */
void atlaas::merge() {
    bool is_vertical;
    size_t index = 0;
    float threshold = variance_factor * variance_mean(dyninter);
    auto it = internal.begin();
    auto st = vertical.begin();

    for (auto& dyninfo : dyninter) {
        if ( dyninfo[N_POINTS] > 0 ) {

            is_vertical = dyninfo[VARIANCE] > threshold;

            if ( (*it)[N_POINTS] < 1 ) {
                *st = is_vertical;
                *it = dyninfo;
            } else if ( *st == is_vertical ) {
                merge(*it, dyninfo);
            } else if ( !*st ) { // was flat
                gndinter[index] = *it;
                *it = dyninfo;
                *st = true;
            } else { // was vertical
                *st = false;
                *it = gndinter[index];
                merge(*it, dyninfo);
            }
            (*it)[LAST_UPDATE] = get_reference_time();
        }

        st++;
        it++;
        index++;
    }
    map_sync = false;
}

void atlaas::merge(cell_info_t& dst, const cell_info_t& src) {
    if ( dst[N_POINTS] < 1 ) {
        dst = src;
        return;
    }
    float z_mean, d_mean, new_n_pts;

    new_n_pts = src[N_POINTS] + dst[N_POINTS];
    z_mean = dst[Z_MEAN];

    if (dst[Z_MAX] < src[Z_MAX])
        dst[Z_MAX] = src[Z_MAX];
    if (dst[Z_MIN] > src[Z_MIN])
        dst[Z_MIN] = src[Z_MIN];

    dst[Z_MEAN] = ( (z_mean * dst[N_POINTS]) + (src[Z_MEAN] * src[N_POINTS]) )
                   / new_n_pts;
    // XXX compute the variance
    //d_mean = dst[Z_MEAN] - z_mean;
    dst[VARIANCE] = ( src[VARIANCE] * src[VARIANCE] * src[N_POINTS]
                    + dst[VARIANCE] * dst[VARIANCE] * dst[N_POINTS]
    //                + d_mean * d_mean * src[N_POINTS] * dst[N_POINTS] / new_n_pts
                    ) / new_n_pts;
    //dst[VARIANCE] = ( dst[VARIANCE] * dst[N_POINTS] + src[VARIANCE] * src[N_POINTS])
    //               / new_n_pts;
    dst[N_POINTS] = new_n_pts;
}

void atlaas::update() {
    // update map from internal
    // internal -> map
    for (size_t idx = 0; idx < internal.size(); idx++) {
        map.bands[N_POINTS][idx]    = internal[idx][N_POINTS];
        map.bands[Z_MAX][idx]       = internal[idx][Z_MAX];
        map.bands[Z_MIN][idx]       = internal[idx][Z_MIN];
        map.bands[Z_MEAN][idx]      = internal[idx][Z_MEAN];
        map.bands[VARIANCE][idx]    = internal[idx][VARIANCE];
        map.bands[LAST_UPDATE][idx] = internal[idx][LAST_UPDATE];
    }
    map_sync = true;
}

void atlaas::_fill_internal() {
    assert( map.names == MAP_NAMES );
    width  = map.get_width();  // x
    height = map.get_height(); // y
    sw = width  / 3; // sub-width
    sh = height / 3; // sub-height
    // set internal size
    internal.resize( width * height );
    // fill internal from map
    // map -> internal
    for (size_t idx = 0; idx < internal.size(); idx++) {
        internal[idx][N_POINTS]     = map.bands[N_POINTS][idx];
        internal[idx][Z_MAX]        = map.bands[Z_MAX][idx];
        internal[idx][Z_MIN]        = map.bands[Z_MIN][idx];
        internal[idx][Z_MEAN]       = map.bands[Z_MEAN][idx];
        internal[idx][VARIANCE]     = map.bands[VARIANCE][idx];
        internal[idx][LAST_UPDATE]  = map.bands[LAST_UPDATE][idx];
    }
    map_sync = true;
}

} // namespace atlaas
