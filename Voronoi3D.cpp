#include "Voronoi3D.hpp"
#ifdef RICH_MPI
#include <mpi.h>
#endif
#include <algorithm>
#include <stack>
#include "Mat33.hpp"
#include "utils.hpp"
#include <fstream>
#include <iostream>
#include <boost/container/flat_map.hpp>
#include "Intersections.hpp"

bool PointInPoly(Tessellation3D const& tess, Vector3D const& point, std::size_t index)
{
	vector<std::size_t> const& faces = tess.GetCellFaces(index);
	std::size_t N = faces.size();
	for (std::size_t i = 0; i < N; ++i)
	{
		Vector3D const& vecref = tess.GetFacePoints()[tess.GetPointsInFace(faces[i])[0]];
		Vector3D normal = CrossProduct(tess.GetFacePoints()[tess.GetPointsInFace(faces[i])[1]] -
			vecref, tess.GetFacePoints()[tess.GetPointsInFace(faces[i])[2]]
			- tess.GetFacePoints()[tess.GetPointsInFace(faces[i])[1]]);
		double sign = ScalarProd(tess.GetMeshPoint(index) - vecref, normal);
		if (ScalarProd(point - vecref, normal)*sign < 0)
			return false;
	}
	return true;
}


namespace
{
	void FirstCheckList(std::stack<std::size_t > &check_stack, vector<bool> &future_check, size_t Norg,
		Delaunay3D const& del)
	{
		check_stack.empty();;
		future_check.resize(Norg, false);
		size_t Ntetra = del.tetras_.size();
		for (size_t i = 0; i < Ntetra; ++i)
		{
			Tetrahedron const& tetra = del.tetras_[i];
			for (size_t j = 0; j < 4; ++j)
			{
				if (tetra.points[j] > Norg)
				{
					for (size_t k = 0; k < 4; ++k)
						if (tetra.points[k] < Norg)
							future_check[tetra.points[k]] = true;
					break;
				}
			}
		}
		for (size_t i = 0; i < Norg; ++i)
			if (future_check[i])
				check_stack.push(i);
	}

	vector<Face> BuildBox(Vector3D const& ll, Vector3D const& ur)
	{
		double dx = ur.x - ll.x;
		double dy = ur.y - ll.y;
		double dz = ur.z - ll.z;
		vector<Face> res(6);
		vector<Vector3D> points;
		points.push_back(ll);
		points.push_back(ll + Vector3D(dx, 0, 0));
		points.push_back(ll + Vector3D(dx, dy, 0));
		points.push_back(ll + Vector3D(0, dy, 0));
		points.push_back(ll + Vector3D(0, 0, dz));
		points.push_back(ll + Vector3D(dx, 0, dz));
		points.push_back(ll + Vector3D(dx, dy, dz));
		points.push_back(ll + Vector3D(0, dy, dz));
		points.push_back(ur);
		res[0].vertices.push_back(points[0]);
		res[0].vertices.push_back(points[1]);
		res[0].vertices.push_back(points[2]);
		res[0].vertices.push_back(points[3]);
		res[1].vertices.push_back(points[0]);
		res[1].vertices.push_back(points[4]);
		res[1].vertices.push_back(points[5]);
		res[1].vertices.push_back(points[1]);
		res[2].vertices.push_back(points[3]);
		res[2].vertices.push_back(points[7]);
		res[2].vertices.push_back(points[4]);
		res[2].vertices.push_back(points[0]);
		res[3].vertices.push_back(points[2]);
		res[3].vertices.push_back(points[6]);
		res[3].vertices.push_back(points[7]);
		res[3].vertices.push_back(points[3]);
		res[4].vertices.push_back(points[1]);
		res[4].vertices.push_back(points[5]);
		res[4].vertices.push_back(points[6]);
		res[4].vertices.push_back(points[2]);
		res[5].vertices.push_back(points[5]);
		res[5].vertices.push_back(points[4]);
		res[5].vertices.push_back(points[7]);
		res[5].vertices.push_back(points[6]);
		return res;
	}

	vector<Vector3D> GetBoxNormals(Vector3D const& ll,Vector3D const& ur)
	{
		vector<Face> faces = BuildBox(ll, ur);
		vector<Vector3D> res(faces.size());
		for (size_t i = 0; i < res.size(); ++i)
			res[i] = CrossProduct(faces[i].vertices[2] - faces[i].vertices[0], faces[i].vertices[1] - faces[i].vertices[0]);
		return res;
	}

	size_t BoxIndex(vector<Vector3D> const& fnormals, Vector3D normal)
	{
		double max_angle = ScalarProd(fnormals[0], normal);
		size_t loc = 0;
		for (size_t i = 1; i < fnormals.size(); ++i)
		{
			double temp = ScalarProd(fnormals[i], normal);
			if (temp > max_angle)
			{
				max_angle = temp;
				loc = i;
			}
		}
		return loc;
	}

	void CleanDuplicates(vector<size_t> &indeces, vector<Vector3D> const& points, vector<size_t> &res, double R)
	{
		res.clear();
		res.push_back(indeces[0]);
		for (size_t i = 1; i < indeces.size(); ++i)
		{
			Vector3D diff = points[indeces[i]] - points[indeces[i - 1]];
			if (ScalarProd(diff, diff) > R*R*1e-14)
				res.push_back(indeces[i]);
		}
		Vector3D diff = points[indeces.back()] - points[indeces[0]];
		if (ScalarProd(diff, diff) < R*R*1e-14)
			res.pop_back();
	}

	size_t SetPointTetras(vector<vector<size_t> > &PointTetras, size_t Norg, vector<Tetrahedron> const& tetras,
		std::set<size_t> const& empty_tetras)
	{
		PointTetras.clear();
		PointTetras.resize(Norg);
		size_t Ntetra = tetras.size();
		size_t bigtet;
		bool has_good, has_big;
		for (size_t i = 0; i < Ntetra; ++i)
		{
			if (empty_tetras.find(i) == empty_tetras.end())
			{
				has_good = false;
				has_big = false;
				for (size_t j = 0; j < 4; ++j)
				{
					size_t temp = tetras[i].points[j];
					if (temp < Norg)
					{
						PointTetras[temp].push_back(i);
						has_good = true;
					}
					else
						//if (temp < Norg + 4)
						has_big = true;
				}
				if (has_big&&has_good)
					bigtet = i;
			}
		}
		return bigtet;
	}

	void MakeRightHandFace(vector<size_t> &indeces, Vector3D const& point, vector<Vector3D> const& face_points,
		vector<size_t> &temp)
	{
		Vector3D V1 = face_points[indeces[1]] - face_points[indeces[0]];
		Vector3D V2 = face_points[indeces.back()] - face_points[indeces[0]];
		if (ScalarProd(CrossProduct(V1, V2), point - face_points[indeces[0]]) < 0)
		{
			temp = indeces;
			for (size_t i = 0; i < temp.size(); ++i)
				indeces[i] = temp[temp.size() - i - 1];
		}
	}

	size_t NextLoopTetra(Tetrahedron const& cur_tetra, size_t last_tetra, size_t N0, size_t N1)
	{
		for (size_t i = 0; i < 4; ++i)
		{
			if (cur_tetra.points[i] != N0&&cur_tetra.points[i] != N1)
				if (cur_tetra.neighbors[i] != last_tetra)
					return cur_tetra.neighbors[i];
		}
		assert(false);
	}

	bool ShouldBuildFace(size_t N0, size_t N1, vector<vector<std::size_t> > const& FacesInCell,
		vector<std::pair<std::size_t, std::size_t> > const& FaceNeighbors, size_t Norg)
	{
		if (N0 >= Norg)
			return false;
		size_t Nfaces = FacesInCell[N0].size();
		for (size_t i = 0; i < Nfaces; ++i)
			if (FaceNeighbors[FacesInCell[N0][i]].second == N1)
				return false;
		return true;
	}

	bool IsOuterTetra(size_t Norg, Tetrahedron const& tetra)
	{
		for (size_t j = 0; j < 4; ++j)
			if (tetra.points[j] >= Norg && tetra.points[j] < (Norg + 4))
				return true;
		return false;
	}

#ifdef RICH_MPI
	std::pair<Vector3D, Vector3D> GetBoundingBox(Tessellation3D const& tproc, int rank)
	{
		vector<Vector3D> const& face_points = tproc.GetFacePoints();
		vector<size_t> faces = tproc.GetCellFaces(static_cast<size_t>(rank));
		Vector3D ll = face_points[tproc.GetPointsInFace(faces[0])[0]];
		Vector3D ur(ll);
		for (size_t i = 0; i < faces.size(); ++i)
		{
			vector<size_t> const& findex = tproc.GetPointsInFace(faces[i]);
			for (size_t j = 0; j < findex.size(); ++j)
			{
				ll.x = std::min(ll.x, face_points[findex[j]].x);
				ll.y = std::min(ll.y, face_points[findex[j]].y);
				ll.z = std::min(ll.z, face_points[findex[j]].z);
				ur.x = std::max(ur.x, face_points[findex[j]].x);
				ur.y = std::max(ur.y, face_points[findex[j]].y);
				ur.z = std::max(ur.z, face_points[findex[j]].z);
			}
		}
		return std::pair<Vector3D, Vector3D>(ll, ur);
	}

	void TalkSymmetry(vector<int> & to_talk_with)
	{
		int wsize;
		MPI_Comm_size(MPI_COMM_WORLD, &wsize);
		vector<int> totalk(static_cast<std::size_t>(wsize), 0);
		vector<int> scounts(totalk.size(), 1);
		for (std::size_t i = 0; i < to_talk_with.size(); ++i)
			totalk[to_talk_with[i]] = 1;
		int nrecv = 0;
		MPI_Reduce_scatter(&totalk[0], &nrecv, &scounts[0], MPI_INT, MPI_SUM,
			MPI_COMM_WORLD);

		vector<MPI_Request> req(to_talk_with.size());
		for (std::size_t i = 0; i < to_talk_with.size(); ++i)
			MPI_Isend(&wsize, 1, MPI_INT, to_talk_with[i], 3, MPI_COMM_WORLD, &req[i]);
		vector<int> talkwithme;
		for (int i = 0; i < nrecv; ++i)
		{
			MPI_Status status;
			MPI_Recv(&wsize, 1, MPI_INT, MPI_ANY_SOURCE, 3, MPI_COMM_WORLD, &status);
			talkwithme.push_back(status.MPI_SOURCE);
		}
		MPI_Waitall(to_talk_with.size(), &req[0], MPI_STATUSES_IGNORE);
		vector<int> new_talk_with_me;
		for (std::size_t i = 0; i < to_talk_with.size(); ++i)
			if (std::find(talkwithme.begin(), talkwithme.end(), to_talk_with[i]) != talkwithme.end())
				new_talk_with_me.push_back(to_talk_with[i]);
		to_talk_with = new_talk_with_me;
	}
#endif //RICH_MPI

	double CalcFaceArea(vector<std::size_t> const& indeces, vector<Vector3D> const& points)
	{
		std::size_t Nloop = indeces.size() - 2;
		Vector3D temp;
		for (std::size_t i = 0; i < Nloop; ++i)
			temp += CrossProduct(points[indeces[i + 2]] - points[indeces[0]], points[indeces[i + 1]] - points[indeces[0]]);
		return 0.5*abs(temp);
	}

	bool PointInVertices(b_array_4 const& points, std::size_t point)
	{
		return !(std::find(points.begin(), points.end(), point) == points.end());
	}

	bool PointInDomain(Vector3D const& ll, Vector3D const& ur, Vector3D const& point)
	{
		if (point.x > ll.x&&point.x<ur.x&&point.y>ll.y&&point.y<ur.y&&point.z>ll.z&&point.z < ur.z)
			return true;
		else
			return false;
	}

	Vector3D MirrorPoint(Face const& face, Vector3D const& point)
	{
		Vector3D normal = CrossProduct(face.vertices[1] - face.vertices[0], face.vertices[2] - face.vertices[0]);
		normal = normal / abs(normal);
		return point - (2 * ScalarProd(point - face.vertices[0], normal))*normal;
	}
}

#ifdef RICH_MPI
vector<Vector3D> Voronoi3D::UpdateMPIPoints(Tessellation3D const& vproc, int rank,
	vector<Vector3D> const& points, vector<std::size_t> &selfindex, vector<int> &sentproc,
	vector<vector<std::size_t> > &sentpoints)
{
	vector<Vector3D> res;
	res.reserve(points.size());
	selfindex.clear();
	std::size_t npoints = points.size();
	std::size_t nproc = vproc.GetPointNo();
	vector<std::size_t> neighbors = vproc.GetNeighbors(static_cast<std::size_t>(rank));
	vector<std::size_t> realneigh;
	sentpoints.clear();
	sentproc.clear();
	for (std::size_t i = 0; i < neighbors.size(); ++i)
		if (static_cast<std::size_t>(neighbors[i]) < nproc)
		{
			realneigh.push_back(neighbors[i]);
			sentproc.push_back(neighbors[i]);
		}
	std::size_t Nreal = realneigh.size();
	sentpoints.resize(sentproc.size());

	for (std::size_t i = 0; i < npoints; ++i)
	{
		Vector3D temp = points[i];
		if (PointInPoly(vproc, temp, static_cast<std::size_t>(rank)))
		{
			res.push_back(temp);
			selfindex.push_back(i);
			continue;
		}
		bool good = false;
		for (std::size_t j = 0; j < Nreal; ++j)
		{
			if (PointInPoly(vproc, temp, realneigh[j]))
			{
				sentpoints[j].push_back(i);
				good = true;
				break;
			}
		}
		if (good)
			continue;
		for (std::size_t j = 0; j < nproc; ++j)
		{
			if (std::find(realneigh.begin(), realneigh.end(), j) != realneigh.end() || j == static_cast<std::size_t>(rank))
				continue;
			if (PointInPoly(vproc, temp, j))
			{
				good = true;
				std::size_t index = std::find(sentproc.begin(), sentproc.end(), j) - sentproc.begin();
				if (index >= sentproc.size())
				{
					sentproc.push_back(static_cast<int>(j));
					sentpoints.push_back(vector<std::size_t>(1, i));
				}
				else
					sentpoints[index].push_back(i);
				break;
			}
		}
		if (good)
			continue;
		UniversalError eo("Point is not inside any processor");
		eo.AddEntry("CPU rank", rank);
		eo.AddEntry("Point number", static_cast<double>(i));
		eo.AddEntry("Point x cor", points[i].x);
		eo.AddEntry("Point y cor", points[i].y);
		throw eo;
	}
	// Send/Recv the points
	// Communication
	int wsize;
	MPI_Comm_size(MPI_COMM_WORLD, &wsize);
	vector<int> totalk(static_cast<std::size_t>(wsize), 0);
	vector<int> scounts(totalk.size(), 1);
	for (std::size_t i = 0; i < sentproc.size(); ++i)
		totalk[sentproc[i]] = 1;
	int nrecv;
	MPI_Reduce_scatter(&totalk[0], &nrecv, &scounts[0], MPI_INT, MPI_SUM,
		MPI_COMM_WORLD);

	vector<MPI_Request> req(sentproc.size());
	for (std::size_t i = 0; i < sentproc.size(); ++i)
		MPI_Isend(&wsize, 1, MPI_INT, sentproc[i], 3, MPI_COMM_WORLD, &req[i]);
	vector<int> talkwithme;
	for (int i = 0; i < nrecv; ++i)
	{
		MPI_Status status;
		MPI_Recv(&wsize, 1, MPI_INT, MPI_ANY_SOURCE, 3, MPI_COMM_WORLD, &status);
		talkwithme.push_back(status.MPI_SOURCE);
	}
	MPI_Waitall(static_cast<int>(req.size()), &req[0], MPI_STATUSES_IGNORE);
	MPI_Barrier(MPI_COMM_WORLD);
	for (std::size_t i = 0; i < talkwithme.size(); ++i)
	{
		if (std::find(sentproc.begin(), sentproc.end(), talkwithme[i]) == sentproc.end())
		{
			sentproc.push_back(talkwithme[i]);
			sentpoints.push_back(vector<std::size_t>());
		}
	}
	// Point exchange
	vector<vector<Vector3D> > incoming = MPI_exchange_data(sentproc, sentpoints, points);
	// Combine the vectors
	for (std::size_t i = 0; i < incoming.size(); ++i)
		for (std::size_t j = 0; j < incoming[i].size(); ++j)
			res.push_back(incoming[i][j]);
	return res;
}
#endif //RICH_MPI


Voronoi3D::Voronoi3D()
{}

Voronoi3D::Voronoi3D(Vector3D const& ll, Vector3D const& ur) :ll_(ll), ur_(ur) {}

void Voronoi3D::CalcRigidCM(std::size_t face_index)
{
	Vector3D normal = normalize(del_.points_[FaceNeighbors_[face_index].first] - del_.points_[FaceNeighbors_[face_index].second]);
	std::size_t real, other;
	if (FaceNeighbors_[face_index].first >= Norg_)
	{
		real = FaceNeighbors_[face_index].second;
		other = FaceNeighbors_[face_index].first;
	}
	else
	{
		real = FaceNeighbors_[face_index].first;
		other = FaceNeighbors_[face_index].second;
	}
	CM_[other] = CM_[real] - 2 * normal*ScalarProd(normal, CM_[real] - tetra_centers_[PointsInFace_[face_index][0]]);
}

vector<Vector3D> Voronoi3D::CreateBoundaryPoints(vector<std::pair<std::size_t, std::size_t> > const& to_duplicate)
{
	vector<Face> faces = BuildBox(ll_, ur_);
	vector<Vector3D> res;
	for (std::size_t i = 0; i < to_duplicate.size(); ++i)
		res.push_back(MirrorPoint(faces[to_duplicate[i].first], del_.points_[to_duplicate[i].second]));
	return res;
}

#ifdef RICH_MPI
vector<Vector3D> Voronoi3D::CreateBoundaryPointsMPI(vector<std::pair<std::size_t, std::size_t> > const& to_duplicate,
	Tessellation3D const& tproc, vector<vector<size_t> > &self_duplicate)
{
	vector<vector<size_t> > to_send;

	vector<Face> box_faces = BuildBox(ll_, ur_);
	vector<Vector3D> box_normals = GetBoxNormals(ll_, ur_);
	vector<vector<size_t> > box_candidates(box_normals.size());
	vector<vector<size_t> > new_self_duplicate(box_normals.size());
	self_duplicate.resize(box_faces.size());

	int rank = 0;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	std::size_t Nproc = tproc.GetPointNo();
	for (std::size_t i = 0; i < to_duplicate.size(); ++i)
	{
		std::pair<std::size_t, std::size_t>const& neigh = tproc.GetFaceNeighbors(to_duplicate[i].first);
		if (neigh.first < Nproc && neigh.first != rank)
		{
			if (std::find(duplicatedprocs_.begin(), duplicatedprocs_.end(), neigh.first) == duplicatedprocs_.end())
				duplicatedprocs_.push_back(neigh.first);
		}
		if (neigh.second < Nproc && neigh.second != rank)
		{
			if (std::find(duplicatedprocs_.begin(), duplicatedprocs_.end(), neigh.second) == duplicatedprocs_.end())
				duplicatedprocs_.push_back(neigh.second);
		}
	}
	TalkSymmetry(duplicatedprocs_);
	to_send.resize(duplicatedprocs_.size());
	duplicated_points_.resize(duplicatedprocs_.size());
	vector<Vector3D> res;
	// Get the indeces and deal with selfboundary
	for (std::size_t i = 0; i < to_duplicate.size(); ++i)
	{
		std::pair<std::size_t, std::size_t>const& neigh = tproc.GetFaceNeighbors(to_duplicate[i].first);
		if (neigh.first != static_cast<std::size_t>(rank))
		{
			if (neigh.first < Nproc)
			{
				std::size_t index = std::find(duplicatedprocs_.begin(), duplicatedprocs_.end(), static_cast<int>(neigh.first))
					- duplicatedprocs_.begin();
				if (index < duplicatedprocs_.size())
					to_send[index].push_back(to_duplicate[i].second);
			}
			else
			{
				Vector3D face_normal = tproc.GetMeshPoint(neigh.first) - tproc.GetMeshPoint(neigh.second);
				size_t index = BoxIndex(box_normals, face_normal);
				box_candidates[index].push_back(to_duplicate[i].second);
			}
		}
		if (neigh.second != static_cast<std::size_t>(rank))
		{
			if (neigh.second < Nproc)
			{
				std::size_t index = std::find(duplicatedprocs_.begin(), duplicatedprocs_.end(), static_cast<int>(neigh.second))
					- duplicatedprocs_.begin();
				if (index < duplicatedprocs_.size())
					to_send[index].push_back(to_duplicate[i].second);
			}
			else
			{
				Vector3D face_normal = tproc.GetMeshPoint(neigh.second) - tproc.GetMeshPoint(neigh.first);
				size_t index = BoxIndex(box_normals, face_normal);
				box_candidates[index].push_back(to_duplicate[i].second);
			}
		}	
	}
	// Clean
	for (size_t i = 0; i < duplicated_points_.size(); ++i)
	{
		std::sort(to_send[i].begin(), to_send[i].end());
		to_send[i] = unique(to_send[i]);
		vector<size_t> temp;
		for (size_t j = 0; j < to_send[i].size(); ++j)
		{
			if (duplicated_points_[i].empty() ||
				!std::binary_search(duplicated_points_[i].begin(), duplicated_points_[i].end(), to_send[i][j]))
				temp.push_back(to_send[i][j]);
		}
		to_send[i] = temp;
		duplicated_points_[i].insert(duplicated_points_[i].end(), temp.begin(), temp.end());
	}
	for (size_t i = 0; i < box_candidates.size(); ++i)
	{
		std::sort(box_candidates[i].begin(), box_candidates[i].end());
		box_candidates[i] = unique(box_candidates[i]);
		vector<size_t> temp;
		for (size_t j = 0; j < box_candidates[i].size(); ++j)
		{
			if (self_duplicate[i].empty() ||
				!std::binary_search(self_duplicate[i].begin(), self_duplicate[i].end(), box_candidates[i][j]))
			{
				temp.push_back(box_candidates[i][j]);
				res.push_back(MirrorPoint(box_faces[i], del_.points_[temp.back()]));
			}
		}
		self_duplicate[i].insert(self_duplicate[i].end(), temp.begin(), temp.end());
		box_candidates[i] = temp;
	}
	// Communicate
	vector<vector<Vector3D> > toadd = MPI_exchange_data(duplicatedprocs_, to_send, del_.points_);
	// Add points
	Nghost_.resize(toadd.size());
	for (std::size_t i = 0; i < toadd.size(); ++i)
		for (std::size_t j = 0; j < toadd[i].size(); ++j)
		{
			Nghost_[i].push_back(Norg_ + 4 + res.size());
			res.push_back(toadd[i][j]);
		}
	return res;
}
#endif //RICH_MPI

vector<vector<std::size_t> > const& Voronoi3D::GetGhostIndeces(void) const
{
	return Nghost_;
}

#ifdef RICH_MPI
void Voronoi3D::Build(vector<Vector3D> const & points, Tessellation3D const& tproc)
{
	assert(points.size() > 0);
	// Clear data
	PointTetras_.clear();
	R_.clear();
	tetra_centers_.clear();
	del_.Clean();
	// Voronoi Data
	FacesInCell_.clear();
	PointsInFace_.clear();
	FaceNeighbors_.clear();
	CM_.clear();
	volume_.clear();
	area_.clear();
	Nghost_.clear();
	duplicatedprocs_.clear();
	duplicated_points_.clear();

	int rank = 0;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	vector<Vector3D> new_points = UpdateMPIPoints(tproc, rank, points, self_index_, sentprocs_, sentpoints_);
	Norg_ = new_points.size();
	std::pair<Vector3D, Vector3D> bounding_box = GetBoundingBox(tproc, rank);
	del_.Build(new_points,bounding_box.second,bounding_box.first);
	R_.resize(del_.tetras_.size());
	std::fill(R_.begin(), R_.end(), -1);
	tetra_centers_.resize(R_.size());
	bigtet_ = SetPointTetras(PointTetras_, Norg_, del_.tetras_, del_.empty_tetras_);

	vector<vector<size_t> > self_duplicate;
	vector<std::pair<std::size_t, std::size_t> > ghost_index = FindIntersections(tproc, false); // intersecting tproc face, point index
	vector<Vector3D> extra_points = CreateBoundaryPointsMPI(ghost_index, tproc,self_duplicate);

	del_.BuildExtra(extra_points);
	R_.resize(del_.tetras_.size());
	std::fill(R_.begin(), R_.end(), -1);
	tetra_centers_.resize(R_.size());
	bigtet_ = SetPointTetras(PointTetras_, Norg_, del_.tetras_, del_.empty_tetras_);

	ghost_index = FindIntersections(tproc, true);
	extra_points = CreateBoundaryPointsMPI(ghost_index, tproc,self_duplicate);

	del_.BuildExtra(extra_points);
	R_.resize(del_.tetras_.size());
	std::fill(R_.begin(), R_.end(), -1);
	tetra_centers_.resize(R_.size());


	CM_.resize(del_.points_.size());
	volume_.resize(Norg_);
	// Create Voronoi
	BuildVoronoi();
	CalcAllCM();
	for (std::size_t i = 0; i < FaceNeighbors_.size(); ++i)
		if (BoundaryFace(i))
			CalcRigidCM(i);
	// communicate the ghost CM
	vector<vector<Vector3D> > incoming = MPI_exchange_data(duplicatedprocs_, duplicated_points_, CM_);
	// Add the recieved CM
	for (size_t i = 0; i < incoming.size(); ++i)
		for (size_t j = 0; j < incoming.at(i).size(); ++j)
			CM_[Nghost_.at(i).at(j)] = incoming[i][j];
}
#endif

void Voronoi3D::CalcAllCM(void)
{
	size_t Nfaces = FaceNeighbors_.size();
	boost::array<Vector3D, 4> tetra;
	for (size_t i = 0; i < Nfaces; ++i)
	{
		size_t N0 = FaceNeighbors_[i].first;
		size_t N1 = FaceNeighbors_[i].second;
		size_t Npoints = PointsInFace_[i].size();

		tetra[0] = tetra_centers_[PointsInFace_[i][0]];
		for (std::size_t j = 0; j < Npoints - 2; ++j)
		{
			tetra[1] = tetra_centers_[PointsInFace_[i][j + 1]];
			tetra[2] = tetra_centers_[PointsInFace_[i][j + 2]];
			if (N0 < Norg_)
			{
				tetra[3] = del_.points_[N0];
				double vol = GetTetraVolume(tetra);
				CM_[N0] += std::abs(vol)*GetTetraCM(tetra);
				volume_[N0] += std::abs(vol);
			}
			if (N1 < Norg_)
			{
				tetra[3] = del_.points_[N1];
				double vol = GetTetraVolume(tetra);
				CM_[N1] += std::abs(vol)*GetTetraCM(tetra);
				volume_[N1] += std::abs(vol);
			}
		}
	}
}

void Voronoi3D::Build(vector<Vector3D> const & points)
{
	assert(points.size() > 0);
	// Clear data
	PointTetras_.clear();
	R_.clear();
	tetra_centers_.clear();
	del_.Clean();
	// Voronoi Data
	FacesInCell_.clear();
	PointsInFace_.clear();
	FaceNeighbors_.clear();
	CM_.clear();
	volume_.clear();
	area_.clear();
	Norg_ = points.size();
	duplicatedprocs_.clear();
	duplicated_points_.clear();
	Nghost_.clear();

	del_.Build(points,ur_,ll_);
	R_.resize(del_.tetras_.size());
	std::fill(R_.begin(), R_.end(), -1);
	tetra_centers_.resize(R_.size());
	bigtet_ = SetPointTetras(PointTetras_, Norg_, del_.tetras_, del_.empty_tetras_);

	vector<std::pair<std::size_t, std::size_t> > ghost_index = SerialFindIntersections();
	vector<Vector3D> extra_points = CreateBoundaryPoints(ghost_index);

	del_.BuildExtra(extra_points);

	R_.resize(del_.tetras_.size());
	std::fill(R_.begin(), R_.end(), -1);
	tetra_centers_.resize(R_.size());

	CM_.resize(del_.points_.size());
	volume_.resize(Norg_, 0);
	// Create Voronoi
	BuildVoronoi();
	CalcAllCM();
	for (std::size_t i = 0; i < FaceNeighbors_.size(); ++i)
		if (BoundaryFace(i))
			CalcRigidCM(i);
}

void Voronoi3D::BuildVoronoi(void)
{
	FacesInCell_.resize(Norg_);
	PointTetras_.resize(Norg_);
	FaceNeighbors_.reserve(Norg_ * 10);
	PointsInFace_.reserve(Norg_ * 10);
	for (size_t i = 0; i < Norg_; ++i)
	{
		FacesInCell_[i].reserve(20);
		PointTetras_[i].reserve(20);
	}
	vector<size_t> temp, temp2;
	// Build all voronoi points
	std::size_t Ntetra = del_.tetras_.size();
	for (size_t i = 0; i < Ntetra; ++i)
		if (del_.empty_tetras_.find(i) == del_.empty_tetras_.end())
		{
			CalcTetraRadiusCenter(i);
		}
	// Organize the faces and assign them to cells
	for (size_t i = 0; i < Ntetra; ++i)
	{
		if (del_.empty_tetras_.find(i) == del_.empty_tetras_.end())
		{
			Tetrahedron const& tetra = del_.tetras_[i];
			if (IsOuterTetra(Norg_, tetra))
				continue;
			for (size_t j = 0; j < 3; ++j)
			{
				for (size_t k = j + 1; k < 4; ++k)
				{
					size_t N0 = tetra.points[j];
					size_t N1 = tetra.points[k];
					if (N0 > N1)
					{
						size_t ttemp = N0;
						N0 = N1;
						N1 = ttemp;
					}
					if (ShouldBuildFace(N0, N1, FacesInCell_, FaceNeighbors_, Norg_))
					{
						temp.clear();
						temp.push_back(i);
						size_t next_check = NextLoopTetra(tetra, i, N0, N1);
						size_t cur_check = next_check;
						size_t last_check = i;
						while (next_check != i)
						{
							Tetrahedron const& tet_check = del_.tetras_[cur_check];
							temp.push_back(cur_check);
							next_check = NextLoopTetra(tet_check, last_check, N0, N1);
							last_check = cur_check;
							cur_check = next_check;
						}
						assert(temp.size() > 2);
						CleanDuplicates(temp, tetra_centers_, temp2, abs(del_.points_[N0] - del_.points_[N1]));
						if (temp2.size() < 3)
							continue;
						temp = temp2;
						FaceNeighbors_.push_back(std::pair<size_t, size_t>(N0, N1));
						PointsInFace_.push_back(temp);
						FacesInCell_[N0].push_back(PointsInFace_.size() - 1);
						if (N1 < Norg_)
							FacesInCell_[N1].push_back(PointsInFace_.size() - 1);
						// Make faces right handed
						MakeRightHandFace(PointsInFace_.back(), del_.points_[N0], tetra_centers_, temp);
						area_.push_back(CalcFaceArea(PointsInFace_.back(), tetra_centers_));
					}
				}
			}
		}
	}

}

double Voronoi3D::GetRadius(std::size_t index)
{
	if (R_[index] < 0)
		R_[index] = CalcTetraRadiusCenter(index);
	return R_[index];
}

double Voronoi3D::GetMaxRadius(std::size_t index)
{
	std::size_t N = PointTetras_[index].size();
	double res = 0;
	for (std::size_t i = 0; i < N; ++i)
		res = std::max(res, GetRadius(PointTetras_[index][i]));
	return 2 * res;
}

vector<std::size_t>  Voronoi3D::FindIntersectionsSingle(vector<Face> const& box, std::size_t point, Sphere &sphere)
{
	std::size_t N = PointTetras_[point].size();
	vector<std::size_t> res;
	res.reserve(4);
	for (std::size_t j = 0; j < box.size(); ++j)
	{
		for (std::size_t i = 0; i < N; ++i)
		{
			sphere.radius = GetRadius(PointTetras_[point][i]);
			sphere.center = tetra_centers_[PointTetras_[point][i]];
			if (FaceSphereIntersections(box[j], sphere))
			{
				res.push_back(j);
				break;
			}
		}
	}
	return res;
}

vector<std::size_t>  Voronoi3D::FindIntersectionsRecursive(Tessellation3D const& tproc, std::size_t rank, std::size_t point,
	Sphere &sphere, bool recursive)
{
	vector<std::size_t> res;
	std::size_t N = tproc.GetPointNo();
	std::size_t Nfaces = tproc.GetTotalFacesNumber();
	vector<bool> visited(Nfaces, false);
	std::stack<std::size_t> to_check;
	std::size_t Ntetra = PointTetras_[point].size();
	vector<std::size_t> faces = tproc.GetCellFaces(rank);
	for (std::size_t i = 0; i < faces.size(); ++i)
		to_check.push(faces[i]);
	while (!to_check.empty())
	{
		std::size_t cur = to_check.top();
		to_check.pop();
		if (visited[cur])
			continue;
		visited[cur] = true;
		Face f(VectorValues(tproc.GetFacePoints(), tproc.GetPointsInFace(cur)), tproc.GetFaceNeighbors(cur).first,
			tproc.GetFaceNeighbors(cur).second);
		for (std::size_t j = 0; j < Ntetra; ++j)
		{
			sphere.radius = GetRadius(PointTetras_[point][j]);
			sphere.center = tetra_centers_[PointTetras_[point][j]];
			if (FaceSphereIntersections(f, sphere))
			{
				res.push_back(cur);
				if (recursive)
				{
					if (f.neighbors.first < N && f.neighbors.first != rank)
					{
						vector<std::size_t> const& faces_temp = tproc.GetCellFaces(f.neighbors.first);
						for (std::size_t i = 0; i < faces_temp.size(); ++i)
							if (!visited[faces_temp[i]])
								to_check.push(faces_temp[i]);
					}
					if (f.neighbors.second < N && f.neighbors.second != rank)
					{
						vector<std::size_t> const& faces_temp = tproc.GetCellFaces(f.neighbors.second);
						for (std::size_t i = 0; i < faces_temp.size(); ++i)
							if (!visited[faces_temp[i]])
								to_check.push(faces_temp[i]);
					}
				}
				break;
			}
		}
	}
	std::sort(res.begin(), res.end());
	return unique(res);
}


void Voronoi3D::GetPointToCheck(std::size_t point, vector<bool> const& checked, vector<std::size_t> &res)
{
	res.clear();
	std::size_t ntetra = PointTetras_[point].size();
	for (std::size_t i = 0; i < ntetra; ++i)
	{
		for (std::size_t j = 0; j < 4; ++j)
			if (del_.tetras_[PointTetras_[point][i]].points[j] < Norg_ && !checked[del_.tetras_[PointTetras_[point][i]].points[j]])
				res.push_back(del_.tetras_[PointTetras_[point][i]].points[j]);
	}
	std::sort(res.begin(), res.end());
	res = unique(res);
}

std::size_t Voronoi3D::GetFirstPointToCheck(void)const
{
	for (std::size_t i = 0; i < 4; ++i)
		if (del_.tetras_[bigtet_].points[i] < Norg_)
			return del_.tetras_[bigtet_].points[i];
	throw UniversalError("Can't find first point to start boundary search");
}

#ifdef RICH_MPI
vector<std::pair<std::size_t, std::size_t> > Voronoi3D::FindIntersections(Tessellation3D const& tproc, bool recursive)
{
	int rank;
	MPI_Comm_rank(MPI_COMM_WORLD, &rank);
	vector<Face> box = BuildBox(ll_, ur_);
	//std::size_t cur_loc = GetFirstPointToCheck();
	std::stack<std::size_t > check_stack;
	vector<std::size_t> point_neigh;
	std::size_t nbox = box.size();
	vector<std::pair<std::size_t, std::size_t> > res;
	Sphere sphere;
	vector<bool> checked(Norg_, false), will_check(Norg_, false);
	FirstCheckList(check_stack, will_check, Norg_, del_);
	size_t cur_loc;
	while (!check_stack.empty())
	{
		cur_loc = check_stack.top();
		check_stack.pop();
		checked[cur_loc] = true;
		// Does sphere have any intersections?
		bool added = false;
		vector<std::size_t> intersecting_faces = FindIntersectionsRecursive(tproc, static_cast<std::size_t>(rank), cur_loc, sphere, recursive);
		if (!intersecting_faces.empty())
		{
			added = true;
			for (std::size_t j = 0; j < intersecting_faces.size(); ++j)
				res.push_back(std::pair<std::size_t, std::size_t>(intersecting_faces[j], cur_loc));
		}
		if (added)
		{
			GetPointToCheck(cur_loc, checked, point_neigh);
			std::size_t Nneigh = point_neigh.size();
			for (std::size_t j = 0; j < Nneigh; ++j)
				if (point_neigh[j] < Norg_ && !will_check[point_neigh[j]])
				{
					check_stack.push(point_neigh[j]);
					will_check[point_neigh[j]] = true;
				}
		}
	}
	return res;
}
#endif //RICH_MPI

vector<std::pair<std::size_t, std::size_t> > Voronoi3D::SerialFindIntersections()
{
	vector<Face> box = BuildBox(ll_, ur_);
	std::size_t cur_loc = GetFirstPointToCheck();
	std::stack<std::size_t > check_stack;
	vector<std::size_t> point_neigh;
	check_stack.push(cur_loc);
	std::size_t nbox = box.size();
	vector<std::pair<std::size_t, std::size_t> > res;
	Sphere sphere;
	vector<bool> checked(Norg_, false), will_check(Norg_, false);
	will_check[cur_loc] = true;
	while (!check_stack.empty())
	{
		cur_loc = check_stack.top();
		check_stack.pop();
		checked[cur_loc] = true;
		// Does sphere have any intersections?
		bool added = false;
		vector<std::size_t> intersecting_faces = FindIntersectionsSingle(box, cur_loc, sphere);
		if (!intersecting_faces.empty())
		{
			added = true;
			for (std::size_t j = 0; j < intersecting_faces.size(); ++j)
				res.push_back(std::pair<std::size_t, std::size_t>(intersecting_faces[j], cur_loc));
		}
		if (added)
		{
			GetPointToCheck(cur_loc, checked, point_neigh);
			std::size_t Nneigh = point_neigh.size();
			for (std::size_t j = 0; j < Nneigh; ++j)
				if (point_neigh[j] < Norg_ && !will_check[point_neigh[j]])
				{
					check_stack.push(point_neigh[j]);
					will_check[point_neigh[j]] = true;
				}
		}
	}
	return res;
}


double Voronoi3D::CalcTetraRadiusCenter(std::size_t index)
{
	Vector3D v2(del_.points_[del_.tetras_[index].points[1]] - del_.points_[del_.tetras_[index].points[0]]);
	Vector3D v3(del_.points_[del_.tetras_[index].points[2]] - del_.points_[del_.tetras_[index].points[0]]);
	Vector3D v4(del_.points_[del_.tetras_[index].points[3]] - del_.points_[del_.tetras_[index].points[0]]);
	Mat33<double> m_a(v2.x, v2.y, v2.z,
		v3.x, v3.y, v3.z,
		v4.x, v4.y, v4.z);
	double a = m_a.determinant();

	Mat33<double> m_Dx(ScalarProd(v2, v2), v2.y, v2.z,
		ScalarProd(v3, v3), v3.y, v3.z,
		ScalarProd(v4, v4), v4.y, v4.z);
	double Dx = m_Dx.determinant();

	Mat33<double> m_Dy(ScalarProd(v2, v2), v2.x, v2.z,
		ScalarProd(v3, v3), v3.x, v3.z,
		ScalarProd(v4, v4), v4.x, v4.z);
	double Dy = -m_Dy.determinant();

	Mat33<double> m_Dz(ScalarProd(v2, v2), v2.x, v2.y,
		ScalarProd(v3, v3), v3.x, v3.y,
		ScalarProd(v4, v4), v4.x, v4.y);
	double Dz = m_Dz.determinant();

	double c = 0;

	tetra_centers_[index] = Vector3D(Dx / (2 * a), Dy / (2 * a), Dz / (2 * a)) + del_.points_[del_.tetras_[index].points[0]];
	return 0.5*sqrt(Dx*Dx + Dy*Dy + Dz*Dz - 4 * a*c) / std::abs(a);
}

Vector3D Voronoi3D::GetTetraCM(boost::array<Vector3D, 4> const& points)const
{
	Vector3D res;
	for (std::size_t i = 0; i < 4; ++i)
		res += 0.25*points[i];
	return res;
}

double Voronoi3D::GetTetraVolume(boost::array<Vector3D, 4> const& points)const
{
	Mat33<double> mat(points[1].x - points[0].x, points[1].y - points[0].y, points[1].z - points[0].z,
		points[2].x - points[0].x, points[2].y - points[0].y, points[2].z - points[0].z,
		points[3].x - points[0].x, points[3].y - points[0].y, points[3].z - points[0].z);
	double det = mat.determinant();
	return det / 6.0;
}

void Voronoi3D::CalcCellCMVolume(std::size_t index)
{
	volume_[index] = 0;
	CM_[index] = Vector3D();
	std::size_t Nfaces = FacesInCell_[index].size();
	boost::array<Vector3D, 4> tetra;
	tetra[3] = del_.points_[index];
	for (std::size_t i = 0; i < Nfaces; ++i)
	{
		std::size_t face = FacesInCell_[index][i];
		std::size_t Npoints = PointsInFace_[face].size();
		tetra[0] = tetra_centers_[PointsInFace_[face][0]];
		double fvol = 0;
		for (std::size_t j = 0; j < Npoints - 2; ++j)
		{
			tetra[1] = tetra_centers_[PointsInFace_[face][j + 1]];
			tetra[2] = tetra_centers_[PointsInFace_[face][j + 2]];
			double vol = GetTetraVolume(tetra);
			fvol += std::abs(vol);
			CM_[index] += std::abs(vol)*GetTetraCM(tetra);
		}
		volume_[index] += fvol;
	}
	CM_[index] = CM_[index] / volume_[index];
}

namespace
{

	void binary_write_single_int(int n, std::ofstream& fh)
	{
		fh.write(reinterpret_cast<const char*>(&n), sizeof(int));
	}

	void binary_write_single_double(double d, std::ofstream& fh)
	{
		fh.write(reinterpret_cast<const char*>(&d), sizeof(double));
	}

	void binary_write_single_size_t(std::size_t n, std::ofstream& fh)
	{
		fh.write(reinterpret_cast<const char*>(&n), sizeof(std::size_t));
	}
}

void Voronoi3D::output(std::string const& filename)const
{

	std::ofstream file_handle(filename.c_str(), std::ios::binary);
	binary_write_single_int(static_cast<int>(Norg_), file_handle);

	// Points
	for (std::size_t i = 0; i < Norg_; ++i)
	{
		binary_write_single_double(del_.points_[i].x, file_handle);
		binary_write_single_double(del_.points_[i].y, file_handle);
		binary_write_single_double(del_.points_[i].z, file_handle);
	}

	binary_write_single_int(static_cast<int>(tetra_centers_.size()), file_handle);
	// Face Points
	for (std::size_t i = 0; i < tetra_centers_.size(); ++i)
	{
		binary_write_single_double(tetra_centers_[i].x, file_handle);
		binary_write_single_double(tetra_centers_[i].y, file_handle);
		binary_write_single_double(tetra_centers_[i].z, file_handle);
	}

	// Faces in cell
	for (std::size_t i = 0; i < Norg_; ++i)
	{
		binary_write_single_int(static_cast<int>(FacesInCell_[i].size()), file_handle);
		for (std::size_t j = 0; j < FacesInCell_[i].size(); ++j)
			binary_write_single_int(static_cast<int>(FacesInCell_[i][j]), file_handle);
	}

	// Points in Face
	binary_write_single_int(static_cast<int>(PointsInFace_.size()), file_handle);
	for (std::size_t i = 0; i < PointsInFace_.size(); ++i)
	{
		binary_write_single_int(static_cast<int>(PointsInFace_[i].size()), file_handle);
		for (std::size_t j = 0; j < PointsInFace_[i].size(); ++j)
			binary_write_single_int(static_cast<int>(PointsInFace_[i][j]), file_handle);
	}

	file_handle.close();
}

std::size_t Voronoi3D::GetPointNo(void) const
{
	return Norg_;
}

Vector3D Voronoi3D::GetMeshPoint(std::size_t index) const
{
	return del_.points_[index];
}

double Voronoi3D::GetArea(std::size_t index) const
{
	return area_[index];
}

Vector3D const& Voronoi3D::GetCellCM(std::size_t index) const
{
	return CM_[index];
}

std::size_t Voronoi3D::GetTotalFacesNumber(void) const
{
	return FaceNeighbors_.size();
}

double Voronoi3D::GetWidth(std::size_t index) const
{
	return pow(3 * volume_[index] * 0.25 / M_PI, 0.333333333);
}

double Voronoi3D::GetVolume(std::size_t index) const
{
	return volume_[index];
}

vector<std::size_t>const& Voronoi3D::GetCellFaces(std::size_t index) const
{
	return FacesInCell_[index];
}

vector<Vector3D>& Voronoi3D::GetMeshPoints(void)
{
	return del_.points_;
}

vector<std::size_t> Voronoi3D::GetNeighbors(std::size_t index)const
{
	std::size_t N = FacesInCell_[index].size();
	vector<std::size_t> res(N);
	for (std::size_t i = 0; i < N; ++i)
	{
		std::size_t face = FacesInCell_[index][i];
		res[i] = FaceNeighbors_[face].first == index ? FaceNeighbors_[face].second :
			FaceNeighbors_[face].first;
	}
	return res;
}


Tessellation3D* Voronoi3D::clone(void) const
{
	return new Voronoi3D(*this);
}

bool Voronoi3D::NearBoundary(std::size_t index) const
{
	std::size_t N = FacesInCell_[index].size();
	for (std::size_t i = 0; i < N; ++i)
	{
		if (BoundaryFace(FacesInCell_[index][i]))
			return true;
	}
	return false;
}

bool Voronoi3D::BoundaryFace(std::size_t index) const
{
	if (FaceNeighbors_[index].first >= Norg_ || FaceNeighbors_[index].second >= Norg_)
	{
#ifdef RICH_MPI
		if (PointInDomain(ll_, ur_, del_.points_[index]))
			return false;
		else
#endif
			return true;
	}
	else
		return false;
}

vector<vector<std::size_t> >& Voronoi3D::GetDuplicatedPoints(void)
{
	return duplicated_points_;
}

vector<vector<std::size_t> >const& Voronoi3D::GetDuplicatedPoints(void)const
{
	return duplicated_points_;
}

std::size_t Voronoi3D::GetTotalPointNumber(void)const
{
	return del_.points_.size();
}

vector<Vector3D>& Voronoi3D::GetAllCM(void)
{
	return CM_;
}

void Voronoi3D::GetNeighborNeighbors(vector<std::size_t> &result, std::size_t point)const
{
	result.clear();
	result.reserve(70);
	vector<std::size_t> neigh = GetNeighbors(point);
	result = neigh;
	std::size_t N = neigh.size();
	vector<std::size_t> temp;
	for (std::size_t i = 0; i < N; ++i)
	{
		if (neigh[i] < Norg_)
		{
			temp = GetNeighbors(neigh[i]);
			result.insert(result.end(), temp.begin(), temp.end());
		}
	}
	sort(result.begin(), result.end());
	result = unique(result);
}

Vector3D Voronoi3D::Normal(std::size_t faceindex)const
{
	return del_.points_[FaceNeighbors_[faceindex].second] - del_.points_[FaceNeighbors_[faceindex].first];
}

bool Voronoi3D::IsGhostPoint(std::size_t index)const
{
	return index >= Norg_;
}

Vector3D Voronoi3D::FaceCM(std::size_t index)const
{
	std::size_t N = PointsInFace_[index].size();
	Vector3D res = tetra_centers_[PointsInFace_[index][0]];
	for (std::size_t i = 1; i < N; ++i)
		res += tetra_centers_[PointsInFace_[index][i]];
	return res / N;
}

Vector3D Voronoi3D::CalcFaceVelocity(std::size_t index, Vector3D const& v0, Vector3D const& v1)const
{
	std::size_t p0 = FaceNeighbors_[index].first;
	std::size_t p1 = FaceNeighbors_[index].second;
	Vector3D r0 = GetMeshPoint(p0);
	Vector3D r1 = GetMeshPoint(p1);
	Vector3D r_diff = r1 - r0;
	double abs_r_diff = abs(r_diff);

	Vector3D f = FaceCM(index);

	Vector3D delta_w = ScalarProd((v0 - v1), (f - (r1 + r0) / 2)) * r_diff / (abs_r_diff * abs_r_diff);
	Vector3D w = (v0 + v1) / 2 + delta_w;
	return w;
}

vector<Vector3D>const& Voronoi3D::GetFacePoints(void) const
{
	return tetra_centers_;
}

vector<std::size_t>const& Voronoi3D::GetPointsInFace(std::size_t index) const
{
	return PointsInFace_[index];
}

std::pair<std::size_t, std::size_t> Voronoi3D::GetFaceNeighbors(std::size_t face_index)const
{
	return std::pair<std::size_t, std::size_t>(FaceNeighbors_[face_index]);
}

vector<int> Voronoi3D::GetDuplicatedProcs(void)const
{
	return duplicatedprocs_;
}

vector<int> Voronoi3D::GetSentProcs(void)const
{
	return sentprocs_;
}

vector<vector<std::size_t> > const& Voronoi3D::GetSentPoints(void)const
{
	return sentpoints_;
}

vector<std::size_t> const& Voronoi3D::GetSelfIndex(void) const
{
	return self_index_;
}
