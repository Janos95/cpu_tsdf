/*
 * Software License Agreement
 *
 * Copyright (c) 2012-, Open Perception, Inc.
 *  
 * All rights reserved.
 *
 */
#include <cpu_tsdf/tsdf_volume_octree.h>
#include <cpu_tsdf/marching_cubes_tsdf_octree.h>

#include <pcl/console/print.h>
#include <pcl/console/time.h>
#include <pcl/visualization/cloud_viewer.h>
#include <pcl/io/pcd_grabber.h>
#include <pcl/io/pcd_io.h>
#include <pcl/io/ply_io.h>
#include <pcl/io/vtk_lib_io.h>
#include <pcl/pcl_macros.h>
#include <pcl/segmentation/extract_clusters.h>

#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include <string>
#include <vector>

int width_ = 640;
int height_ = 480;
float focal_length_x_ = 525.;
float focal_length_y_ = 525.;
float principal_point_x_ = 319.5;
float principal_point_y_ = 239.5;


pcl::PointCloud<pcl::PointNormal>::Ptr
meshToFaceCloud (const pcl::PolygonMesh &mesh)
{
  pcl::PointCloud<pcl::PointNormal>::Ptr cloud (new pcl::PointCloud<pcl::PointNormal>);
  pcl::PointCloud<pcl::PointXYZ> vertices;
  pcl::fromPCLPointCloud2 (mesh.cloud, vertices);

  for (size_t i = 0; i < mesh.polygons.size (); ++i)
  {
    if (mesh.polygons[i].vertices.size () != 3)
    {
      PCL_ERROR ("Found a polygon of size %d\n", mesh.polygons[i].vertices.size ());
      continue;
    }
    Eigen::Vector3f v0 = vertices.at (mesh.polygons[i].vertices[0]).getVector3fMap ();
    Eigen::Vector3f v1 = vertices.at (mesh.polygons[i].vertices[1]).getVector3fMap ();
    Eigen::Vector3f v2 = vertices.at (mesh.polygons[i].vertices[2]).getVector3fMap ();
    float area = ((v1 - v0).cross (v2 - v0)).norm () / 2. * 1E4;
    Eigen::Vector3f normal = ((v1 - v0).cross (v2 - v0));
    normal.normalize ();
    pcl::PointNormal p_new;
    p_new.getVector3fMap () = (v0 + v1 + v2)/3.;
    p_new.normal_x = normal (0);
    p_new.normal_y = normal (1);
    p_new.normal_z = normal (2);
    cloud->points.push_back (p_new);
  }
  cloud->height = 1;
  cloud->width = cloud->size ();
  return (cloud);
}

void
flattenVertices (pcl::PolygonMesh &mesh, float min_dist = 0.0001)
{
  pcl::PointCloud<pcl::PointXYZ>::Ptr vertices (new pcl::PointCloud<pcl::PointXYZ>);
  pcl::fromPCLPointCloud2 (mesh.cloud, *vertices);
  pcl::search::KdTree<pcl::PointXYZ> vert_tree (true);
  vert_tree.setInputCloud (vertices);
  // Find duplicates
  std::vector<int> vertex_remap (vertices->size (), -1);
  int idx = 0;
  std::vector<int> neighbors;
  std::vector<float> dists;
  pcl::PointCloud<pcl::PointXYZ> vertices_new;
  for (size_t i = 0; i < vertices->size (); i++)
  {
    if (vertex_remap[i] >= 0)
      continue;
    vertex_remap[i] = idx;
    vert_tree.radiusSearch (i, min_dist, neighbors, dists);
    for (size_t j = 1; j < neighbors.size (); j++)
    {
      if (dists[j] < min_dist)
        vertex_remap[neighbors[j]] = idx;
    }
    vertices_new.push_back (vertices->at (i));
    idx++;
  }
  std::vector<size_t> faces_to_remove;
  size_t face_idx = 0;
  for (size_t i = 0; i < mesh.polygons.size (); i++)
  {
    pcl::Vertices &v = mesh.polygons[i];
    for (size_t j = 0; j < v.vertices.size (); j++)
    {
      v.vertices[j] = vertex_remap[v.vertices[j]];
    }
    if (v.vertices[0] == v.vertices[1] || v.vertices[1] == v.vertices[2] || v.vertices[2] == v.vertices[0])
    {
      PCL_INFO ("Degenerate face: (%d, %d, %d)\n", v.vertices[0], v.vertices[1], v.vertices[2]);
    }
    else
    {
      mesh.polygons[face_idx++] = mesh.polygons[i];
    }
  }
  mesh.polygons.resize (face_idx);
  pcl::toPCLPointCloud2 (vertices_new, mesh.cloud);
}

void
cleanupMesh (pcl::PolygonMesh &mesh, float face_dist=0.02, int min_neighbors=5)
{
  // Remove faces which aren't within 2 marching cube widths from any others
  pcl::PointCloud<pcl::PointNormal>::Ptr faces = meshToFaceCloud (mesh);
  std::vector<size_t> faces_to_remove;
  pcl::search::KdTree<pcl::PointNormal>::Ptr face_tree (new pcl::search::KdTree<pcl::PointNormal>);
  face_tree->setInputCloud (faces);
  // Find small clusters and remove them
  std::vector<pcl::PointIndices> clusters;
  pcl::EuclideanClusterExtraction<pcl::PointNormal> extractor;
  extractor.setInputCloud (faces);
  extractor.setSearchMethod (face_tree);
  extractor.setClusterTolerance (face_dist);
  extractor.setMaxClusterSize(min_neighbors);
  extractor.extract(clusters);
  PCL_INFO ("Found %d clusters\n", clusters.size ());
  // Aggregate indices
  std::vector<bool> keep_face (faces->size (), false);
  for(size_t i = 0; i < clusters.size(); i++)
  {
    for(size_t j = 0; j < clusters[i].indices.size(); j++)
    {
      faces_to_remove.push_back (clusters[i].indices[j]);
    }
  }
  std::sort (faces_to_remove.begin (), faces_to_remove.end ());
  // Remove the face
  for (ssize_t i = faces_to_remove.size () - 1; i >= 0; i--)
  {
    mesh.polygons.erase (mesh.polygons.begin () + faces_to_remove[i]);
  }
  // Remove all vertices with no face
  pcl::PointCloud<pcl::PointXYZ> vertices;
  pcl::fromPCLPointCloud2 (mesh.cloud, vertices);
  std::vector<bool> has_face (vertices.size (), false);
  for (size_t i = 0; i < mesh.polygons.size (); i++)
  {
    const pcl::Vertices& v = mesh.polygons[i];
    has_face[v.vertices[0]] = true;
    has_face[v.vertices[1]] = true;
    has_face[v.vertices[2]] = true;
  }
  pcl::PointCloud<pcl::PointXYZ> vertices_new;
  std::vector<size_t> get_new_idx (vertices.size ());
  size_t cur_idx = 0;
  for (size_t i = 0; i <vertices.size (); i++)
  {
    if (has_face[i])
    {
      vertices_new.push_back (vertices[i]);
      get_new_idx[i] = cur_idx++;
    }
  }
  for (size_t i = 0; i < mesh.polygons.size (); i++)
  {
    pcl::Vertices &v = mesh.polygons[i];
    v.vertices[0] = get_new_idx[v.vertices[0]];
    v.vertices[1] = get_new_idx[v.vertices[1]];
    v.vertices[2] = get_new_idx[v.vertices[2]];
  }
  pcl::toPCLPointCloud2 (vertices_new, mesh.cloud);
}

bool
reprojectPoint (const pcl::PointXYZRGBA &pt, int &u, int &v)
{
  u = (pt.x * focal_length_x_ / pt.z) + principal_point_x_;
  v = (pt.y * focal_length_y_ / pt.z) + principal_point_y_;
  return (!pcl_isnan (pt.z) && pt.z > 0 && u >= 0 && u < width_ && v >= 0 && v < height_);
}

void
remapCloud (pcl::PointCloud<pcl::PointXYZRGBA>::ConstPtr cloud, 
            const Eigen::Affine3d &pose, 
            pcl::PointCloud<pcl::PointXYZRGBA> &cloud_remapped)
{
  // Reproject
  cloud_remapped = pcl::PointCloud<pcl::PointXYZRGBA> (width_, height_);
  // Initialize to nan
#pragma omp parallel for
  for (size_t i = 0; i < cloud_remapped.size (); i++)
  {
    cloud_remapped[i].z = std::numeric_limits<float>::quiet_NaN ();
  }
  cloud_remapped.is_dense = false;
  for (size_t i = 0; i < cloud->size (); i++)
  {
    const pcl::PointXYZRGBA &pt = cloud->at (i); 
    int u, v;
    if (!reprojectPoint (pt, u, v))
      continue;
    pcl::PointXYZRGBA &pt_remapped = cloud_remapped (u, v);
    if (pcl_isnan (pt_remapped.z) || pt_remapped.z > pt.z)
      pt_remapped = pt;
  }
}

int
main (int argc, char** argv)
{
  namespace bpo = boost::program_options;
  namespace bfs = boost::filesystem;
  bpo::options_description opts_desc("Allowed options");
  bpo::positional_options_description p;

  opts_desc.add_options()
    ("help,h", "produce help message")
    ("in", bpo::value<std::string> ()->required (), "Input dir")
    ("out", bpo::value<std::string> ()->required (), "Output dir")
    ("volume-size", bpo::value<float> (), "Volume size")
    ("cell-size", bpo::value<float> (), "Cell size")
    ("visualize", "Visualize")
    ("verbose", "Verbose")
    ("flatten", "Flatten mesh vertices")
    ("cleanup", "Clean up mesh")
    ("invert", "Transforms are inverted (world -> camera)")
    ("world", "Clouds are given in the world frame")
    ("organized", "Clouds are already organized")
    ("width", bpo::value<int> (), "Image width")
    ("height", bpo::value<int> (), "Image height")
    ("zero-nans", "Nans are represented as (0,0,0)")
    ("num-random-splits", bpo::value<int> (), "Number of random points to sample around each surface reading. Leave empty unless you know what you're doing.")
    ("fx", bpo::value<float> (), "Focal length x")
    ("fy", bpo::value<float> (), "Focal length y")
    ("cx", bpo::value<float> (), "Center pixel x")
    ("cy", bpo::value<float> (), "Center pixel y")
    ("save-ascii", "Save ply file as ASCII rather than binary")
    ;
     
  bpo::variables_map opts;
  bpo::store(bpo::parse_command_line(argc, argv, opts_desc, bpo::command_line_style::unix_style ^ bpo::command_line_style::allow_short), opts);
  bool badargs = false;
  try { bpo::notify(opts); }
  catch(...) { badargs = true; }
  if(opts.count("help") || badargs) {
    cout << "Usage: " << bfs::basename(argv[0]) << " --in [in_dir] --out [out_dir] [OPTS]" << endl;
    cout << "Integrates multiple clouds and returns a mesh. Assumes clouds are PCD files and poses are ascii (.txt) or binary float (.transform) files with the same prefix, specifying the pose of the camera in the world frame. Can customize many parameters, but if you don't know what they do, the defaults are strongly recommended." << endl;
    cout << endl;
    cout << opts_desc << endl;
    return (1);
  }

  // Visualize?
  bool visualize = opts.count ("visualize");
  bool verbose = opts.count ("verbose");
  bool flatten = opts.count ("flatten");
  bool cleanup = opts.count ("cleanup");
  bool invert = opts.count ("invert");
  bool organized = opts.count ("organized");
  bool world_frame = opts.count ("world");
  bool zero_nans = opts.count ("zero-nans");
  bool save_ascii = opts.count ("save-ascii");
  int num_random_splits = 1;
  if (opts.count ("num-random-splits"))
    num_random_splits = opts["num-random-splits"].as<int> ();
  bool binary_poses = false;
  if (opts.count ("width"))
    width_ = opts["width"].as<int> ();
  if (opts.count ("height"))
    height_ = opts["height"].as<int> ();
  focal_length_x_ = 525. * width_ / 640.;
  focal_length_y_ = 525. * height_ / 480.;
  principal_point_x_ = static_cast<float> (width_)/2. - 0.5;
  principal_point_y_ = static_cast<float> (height_)/2. - 0.5;

  if (opts.count ("fx"))
    focal_length_x_ = opts["fx"].as<float> ();
  if (opts.count ("fy"))
    focal_length_y_ = opts["fy"].as<float> ();
  if (opts.count ("cx"))
    principal_point_x_ = opts["cx"].as<float> ();
  if (opts.count ("cy"))
    principal_point_y_ = opts["cy"].as<float> ();

  pcl::console::TicToc tt;
  tt.tic ();
  // Scrape files
  std::vector<std::string> pcd_files;
  std::vector<std::string> pose_files;
  std::string dir = opts["in"].as<std::string> ();
  std::string out_dir = opts["out"].as<std::string> ();
  boost::filesystem::directory_iterator end_itr;
  for (boost::filesystem::directory_iterator itr (dir); itr != end_itr; ++itr)
  {
    std::string extension = boost::algorithm::to_upper_copy
      (boost::filesystem::extension (itr->path ()));
    std::string basename = boost::filesystem::basename (itr->path ());
    std::string pathname = itr->path ().string ();
    if (extension == ".pcd" || extension == ".PCD")
    {
      pcd_files.push_back (pathname);
    }
    else if (extension == ".transform" || extension == ".TRANSFORM")
    {
      pose_files.push_back (pathname);
      binary_poses = true;
    }
    else if (extension == ".txt" || extension == ".TXT")
    {
      pose_files.push_back (pathname);
      binary_poses = false;
    }
  }
  std::sort (pcd_files.begin (), pcd_files.end ());
  std::sort (pose_files.begin (), pose_files.end ());
  PCL_INFO ("Reading in %s pose files\n", 
            binary_poses ? "binary" : "ascii");
  std::vector<Eigen::Affine3d> poses (pose_files.size ());
  for (size_t i = 0; i < pose_files.size (); i++)
  {
    ifstream f (pose_files[i].c_str ());
    float v;
    Eigen::Matrix4d mat;
    for (int y = 0; y < 4; y++)
    {
      for (int x = 0; x < 4; x++)
      {
        if (binary_poses)
          f.read ((char*)&v, sizeof (float));
        else
          f >> v;
        mat (y,x) = static_cast<double> (v);
      }
    }
    f.close ();
    poses[i] = mat;
    if (invert)
      poses[i] = poses[i].inverse ();
    if (verbose)
    {
      std::cout << "Pose[" << i << "]" << std::endl 
                << poses[i].matrix () << std::endl;
    }
  }
  PCL_INFO ("Done!\n");

  // Begin Integration
  float tsdf_size = 12.;
  if (opts.count ("volume-size"))
    tsdf_size = opts["volume-size"].as<float> ();
  float cell_size = 0.006;
  if (opts.count ("cell-size"))
    cell_size = opts["cell-size"].as<float> ();
  int tsdf_res;
  int desired_res = tsdf_size / cell_size;
  // Snap to nearest power of 2;
  int n = 1;
  while (desired_res > n)
  {
    n *= 2;
  }
  tsdf_res = n;
  // Initialize
  cpu_tsdf::TSDFVolumeOctree::Ptr tsdf (new cpu_tsdf::TSDFVolumeOctree);
  tsdf->setGridSize (tsdf_size, tsdf_size, tsdf_size);
  tsdf->setResolution (tsdf_res, tsdf_res, tsdf_res);
  tsdf->setImageSize (width_, height_);
  tsdf->setCameraIntrinsics (focal_length_x_, focal_length_y_, principal_point_x_, principal_point_y_);
  tsdf->setNumRandomSplts (num_random_splits);
  tsdf->reset ();
  // Load data
  pcl::PointCloud<pcl::PointXYZRGBA>::Ptr map (new pcl::PointCloud<pcl::PointXYZRGBA>);
  // Set up visualization
  pcl::visualization::PCLVisualizer::Ptr vis;
  if (visualize)
  {
     vis.reset (new pcl::visualization::PCLVisualizer);
     vis->addCoordinateSystem ();
  } 
  for (size_t i = 0; i < pcd_files.size (); i++)
  {
    PCL_INFO ("On frame %d / %d\n", i+1, pcd_files.size ());
    PCL_INFO ("Cloud: %s, pose: %s\n", 
        pcd_files[i].c_str (), pose_files[i].c_str ());
    pcl::PointCloud<pcl::PointXYZRGBA>::Ptr cloud (new pcl::PointCloud<pcl::PointXYZRGBA>);
    pcl::io::loadPCDFile (pcd_files[i], *cloud);
    // Remove nans
    if (zero_nans)
    {
      for (size_t j = 0; j < cloud->size (); j++)
      {
        if (cloud->at (j).x == 0 && cloud->at (j).y == 0 && cloud->at (j).z == 0)
          cloud->at (j).x = cloud->at (j).y = cloud->at (j).z = std::numeric_limits<float>::quiet_NaN ();
      }
    }
    // Transform
    if (world_frame)
      pcl::transformPointCloud (*cloud, *cloud, poses[i].inverse ());
    // Make organized
    pcl::PointCloud<pcl::PointXYZRGBA>::Ptr cloud_organized (new pcl::PointCloud<pcl::PointXYZRGBA> (width_, height_));
    if (organized)
    {
      pcl::copyPointCloud (*cloud, *cloud_organized);
    }
    else
    {
      for (size_t j = 0; j < cloud_organized->size (); j++)
        cloud_organized->at (j).z = std::numeric_limits<float>::quiet_NaN ();
      for (size_t j = 0; j < cloud->size (); j++)
      {
        const pcl::PointXYZRGBA &pt = cloud->at (j);
        int u, v;
        if (reprojectPoint (pt, u, v))
        {
          pcl::PointXYZRGBA &pt_old = (*cloud_organized) (u, v);
          if (pcl_isnan (pt_old.z) || (pt_old.z > pt.z))
            pt_old = pt;
        }
      }
    }
    if (visualize)
    {
      vis->removeAllPointClouds ();
      pcl::PointCloud<pcl::PointXYZRGBA> cloud_trans;
      pcl::transformPointCloud (*cloud_organized, cloud_trans, poses[i]);
      *map += cloud_trans;
      pcl::visualization::PointCloudColorHandlerCustom<pcl::PointXYZRGBA> map_handler (map, 255, 0, 0);
      vis->addPointCloud (map, map_handler, "map");
      PCL_INFO ("Map\n");
      vis->spin ();
    }
    //Integrate
    tsdf->integrateCloud (*cloud_organized, pcl::PointCloud<pcl::Normal> (), poses[i]);
  }
  // Save
  boost::filesystem::create_directory (out_dir);
  cpu_tsdf::MarchingCubesTSDFOctree mc;
  mc.setInputTSDF (tsdf);
  pcl::PolygonMesh::Ptr mesh (new pcl::PolygonMesh);
  mc.reconstruct (*mesh);
  if (flatten)
    flattenVertices (*mesh);
  if (cleanup)
    cleanupMesh (*mesh);
  PCL_INFO ("Entire pipeline took %f ms\n", tt.toc ());
  if (save_ascii)
    pcl::io::savePLYFile (out_dir + "/mesh.ply", *mesh);
  else
    pcl::io::savePLYFileBinary (out_dir + "/mesh.ply", *mesh);
}

  

