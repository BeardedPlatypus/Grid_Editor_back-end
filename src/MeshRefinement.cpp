//---- GPL ---------------------------------------------------------------------
//
// Copyright (C)  Stichting Deltares, 2011-2020.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation version 3.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// contact: delft3d.support@deltares.nl
// Stichting Deltares
// P.O. Box 177
// 2600 MH Delft, The Netherlands
//
// All indications and logos of, and references to, "Delft3D" and "Deltares"
// are registered trademarks of Stichting Deltares, and remain the property of
// Stichting Deltares. All rights reserved.
//
//------------------------------------------------------------------------------

#include <vector>
#include <cmath>
#include <numeric>
#include <algorithm>
#include "MeshRefinement.hpp"
#include "Mesh.hpp"
#include "Entities.hpp"
#include "SpatialTrees.hpp"
#include "Operations.cpp"

GridGeom::MeshRefinement::MeshRefinement(std::shared_ptr<Mesh> mesh) :
    m_mesh(mesh)
{
    m_mesh->Administrate(Mesh::AdministrationOptions::AdministrateMeshEdgesAndFaces);

    // all faces and edges refined
    m_faceMask.resize(m_mesh->GetNumFaces(), 1);
    m_edgeMask.resize(m_mesh->GetNumEdges(), -1);

    // default refinement 
    m_refinementType = RefinementType::WaveCourant;
};

bool GridGeom::MeshRefinement::Refine(std::vector<Sample>& sample,
                                      const Polygons& polygon,
                                      GridGeomApi::SampleRefineParametersNative& sampleRefineParametersNative,
                                      GridGeomApi::InterpolationParametersNative& interpolationParametersNative)
{

    bool isRefinementBasedOnSamples = false;
    if (!sample.empty())
    {
        isRefinementBasedOnSamples = true;

        m_subtractedSample.resize(sample.size(), false);
        // build the R-Tree
        std::vector<Point> points(sample.size());
        for (int i = 0; i < sample.size(); i++)
        {
            points[i] = { sample[i].x, sample[i].y };
        }
        m_samplesRTree.BuildTree(points, m_mesh->m_projection);

        m_deltaTimeMaxCourant = sampleRefineParametersNative.MinimumCellSize / std::sqrt(gravity);
        m_refineOutsideFace = sampleRefineParametersNative.AccountForSamplesOutside == 1 ? true : false;
        m_minimumFaceSize = sampleRefineParametersNative.MinimumCellSize;
        m_connectHangingNodes = sampleRefineParametersNative.ConnectHangingNodes == 1 ? true : false;

        if (sampleRefineParametersNative.RefinementType == 1)
        {
            m_refinementType = RefinementType::RidgeRefinement;

        }
        if (sampleRefineParametersNative.RefinementType == 2)
        {
            m_refinementType = RefinementType::WaveCourant;

        }
        if (sampleRefineParametersNative.RefinementType == 3)
        {
            m_refinementType = RefinementType::RefinementLevels;
        }
    }

    m_maxNumberOfRefinementIterations = interpolationParametersNative.MaxNumberOfRefinementIterations;

    // get bounding box
    Point lowerLeft{ doubleMissingValue, doubleMissingValue };
    Point upperRight{ doubleMissingValue, doubleMissingValue };
    if (m_mesh->m_projection == Projections::spherical)
    {
        bool successful = m_mesh->GetBoundingBox(lowerLeft, upperRight);
        if (!successful)
        {
            return false;
        }
    }

    // select the nodes to refine
    if (!isRefinementBasedOnSamples && interpolationParametersNative.RefineIntersected)
    {
        m_refineIntersectedFaces = true;
        m_mesh->MaskFaceEdgesInPolygon(polygon, false, true);
        m_mesh->ComputeNodeMaskFromEdgeMask();
    }
    else
    {
        m_mesh->MaskNodesInPolygons(polygon, true);
    }

    bool successful = FindBrotherEdges();
    if (!successful)
    {
        return false;
    }

    //set_initial_mask
    successful = ComputeNodeMaskAtPolygonPerimeter();
    if (!successful)
    {
        return false;
    }

    auto numFacesAfterRefinement = m_mesh->GetNumFaces();
    for (int level = 0; level < m_maxNumberOfRefinementIterations; level++)
    {

        if (level > 0) 
        {
            bool successful = FindBrotherEdges();
            if (!successful)
            {
                return false;
            }
        }

        // Compute all edge lengths at once
        m_mesh->ComputeEdgeLengths();

        const auto numEdgesBeforeRefinement = m_mesh->GetNumEdges();

        // computes the edge and face refinement mask from samples
        if(isRefinementBasedOnSamples)
        {
            bool successful = ComputeRefinementMasksFromSamples(sample);
            if (!successful)
            {
                return false;
            }
            
            for (int i = 0; i < m_edgeMask.size(); i++)
            {
                m_edgeMask[i] = -m_edgeMask[i];
            }

            successful = SmoothEdgeRefinementMask();
            if (!successful)
            {
                return false;
            }
        }
        else
        {
            std::fill(m_faceMask.begin(), m_faceMask.end(), 1);
            std::fill(m_edgeMask.begin(), m_edgeMask.end(), -1);
        }

        if (level == 0)
        {
            //if one face node is in polygon enable face refinement
            for (int f = 0; f < m_mesh->GetNumFaces(); f++)
            {
                bool activeNodeFound = false;
                for (int n = 0; n < m_mesh->GetNumFaceEdges(f); n++)
                {
                    const auto nodeIndex = m_mesh->m_facesNodes[f][n];
                    if (m_mesh->m_nodeMask[nodeIndex] != 0 && m_mesh->m_nodeMask[nodeIndex] != -2)
                    {
                        activeNodeFound = true;
                        break;
                    }
                }
                if (!activeNodeFound)
                {
                    m_faceMask[f] = 0;
                }
            }
        }
        if (level > 0)
        {
            //if one face node is not in polygon disable refinement
            for (int f = 0; f < m_mesh->GetNumFaces(); f++)
            {
                for (int n = 0; n < m_mesh->GetNumFaceEdges(f); n++)
                {
                    const auto nodeIndex = m_mesh->m_facesNodes[f][n];
                    if (m_mesh->m_nodeMask[nodeIndex] != 1)
                    {
                        m_faceMask[f] = 0;
                        break;
                    }
                }
            }
        }

        successful = ComputeEdgesRefinementMask();
        if (!successful)
        {
            return false;
        }

        successful = ComputeIfFaceShouldBeSplitted();
        if (!successful)
        {
            return false;
        }
        int numFacesToRefine = 0;
        for (auto f = 0; f < m_mesh->GetNumFaces(); f++)
        {
            if (m_faceMask[f] != 0)
            {
                numFacesToRefine++;
            }
        }
        if (numFacesToRefine == 0)
        {
            break;
        }
        numFacesAfterRefinement = numFacesAfterRefinement * 4;

        // spit the edges
        successful = RefineFacesBySplittingEdges(numEdgesBeforeRefinement);
        if (!successful)
        {
            return false;
        }

        successful = m_mesh->OffsetSphericalCoordinates(lowerLeft.x, upperRight.x);
        if (!successful)
        {
            return false;
        }

        m_mesh->Administrate(Mesh::AdministrationOptions::AdministrateMeshEdgesAndFaces);

        m_faceMask.resize(m_mesh->GetNumFaces());
        m_edgeMask.resize(m_mesh->GetNumEdges());
    }

    //remove isolated hanging nodes and connect if needed
    if(m_connectHangingNodes)
    {
        auto numRemovedIsolatedHangingNodes = 0;
        auto successful = RemoveIsolatedHangingnodes(numRemovedIsolatedHangingNodes);
        if (!successful)
        {
            return false;
        }

        successful = ConnectHangingNodes();
        if (!successful)
        {
            return false;
        }

        m_mesh->Administrate(Mesh::AdministrationOptions::AdministrateMeshEdgesAndFaces);
    }

  
    return true;
}

bool GridGeom::MeshRefinement::RemoveIsolatedHangingnodes(int& numRemovedIsolatedHangingNodes)
{

    numRemovedIsolatedHangingNodes = 0;
    for (int e = 0; e < m_mesh->GetNumEdges(); ++e)
    {
        const auto brotherEdgeIndex = m_brotherEdges[e];
        if (brotherEdgeIndex < 0)
        {
            continue;
        }

        int commonNode;
        const auto successful = m_mesh->FindCommonNode(e, brotherEdgeIndex, commonNode);
        if (!successful)
        {
            return false;
        }

        if (commonNode > 0 && m_mesh->m_nodesNumEdges[commonNode] == 2)
        {
            for (int f = 0; f < m_mesh->m_edgesNumFaces[e]; ++f)
            {
                const auto faceIndex = m_mesh->m_edgesFaces[e][f];

                //remove_isolated_hanging_nodes: error
                if (faceIndex != m_mesh->m_edgesFaces[brotherEdgeIndex][0] &&
                    faceIndex != m_mesh->m_edgesFaces[brotherEdgeIndex][std::min(m_mesh->m_edgesNumFaces[brotherEdgeIndex], 1)])
                {
                    return true;
                }

                int ee = 0;
                int nn = 0;
                for (int n = 0; n < m_mesh->GetNumFaceEdges(faceIndex); ++n)
                {
                    auto edgeIndex = m_mesh->m_facesEdges[faceIndex][n];
                    if (edgeIndex != brotherEdgeIndex)
                    {
                        m_mesh->m_facesEdges[faceIndex][ee] = edgeIndex;
                        ee++;
                    }

                    auto nodeIndex = m_mesh->m_facesEdges[faceIndex][n];
                    if (nodeIndex != commonNode)
                    {
                        m_mesh->m_facesNodes[faceIndex][nn] = nodeIndex;
                        nn++;
                    }
                }

                m_mesh->m_numFacesNodes[faceIndex] -= 1;

                //remove_isolated_hanging_nodes: error
                if (m_mesh->m_numFacesNodes[faceIndex] != ee || m_mesh->m_numFacesNodes[faceIndex] != nn)
                {
                    return true;
                }
            }

            const auto otherNodeIndex = m_mesh->m_edges[brotherEdgeIndex].first + m_mesh->m_edges[brotherEdgeIndex].second - commonNode;

            //update lin admin
            if (m_mesh->m_edges[e].first == commonNode)
            {
                m_mesh->m_edges[e].first = otherNodeIndex;
            }
            else
            {
                m_mesh->m_edges[e].second = otherNodeIndex;
            }

            //change nod adm of other node
            for (int ee = 0; ee < m_mesh->m_nodesNumEdges[otherNodeIndex]; ++ee)
            {
                if (m_mesh->m_nodesEdges[otherNodeIndex][ee] == brotherEdgeIndex)
                {
                    m_mesh->m_nodesEdges[otherNodeIndex][ee] = e;
                    break;
                }
            }

            //delete node
            //m_mesh->m_nodesNumEdges[commonNode] = 0;
            m_mesh->DeleteNode(commonNode);

            m_mesh->DeleteEdge(brotherEdgeIndex);

            m_brotherEdges[brotherEdgeIndex] = intMissingValue;

            numRemovedIsolatedHangingNodes++;
        }
    }

    return true;
}


bool GridGeom::MeshRefinement::ConnectHangingNodes()
{
    std::vector<int> edgeEndNodeCache(maximumNumberOfNodesPerFace, intMissingValue);
    std::vector<int> hangingNodeCache(maximumNumberOfNodesPerFace, intMissingValue);
    bool successful = true;
    for (int f = 0; f < m_mesh->GetNumFaces(); ++f)
    {
        std::fill(edgeEndNodeCache.begin(), edgeEndNodeCache.end(), intMissingValue);
        std::fill(hangingNodeCache.begin(), hangingNodeCache.end(), intMissingValue);
        auto numEdges = m_mesh->GetNumFaceEdges(f);
        if (numEdges > maximumNumberOfNodesPerFace)
        {
            continue;
        }

        int numNonHangingNodes = 0;
        for (int n = 0; n < numEdges; ++n)
        {
            auto e = NextCircularBackwardIndex(n, numEdges);
            auto ee = NextCircularForwardIndex(n, numEdges);

            const auto edgeIndex = m_mesh->m_facesEdges[f][n];
            const auto firstEdgeIndex = m_mesh->m_facesEdges[f][e];
            const auto secondEdgeIndex = m_mesh->m_facesEdges[f][ee];
            if (m_brotherEdges[edgeIndex] != secondEdgeIndex)
            {

                if (numNonHangingNodes > maximumNumberOfNodesPerFace - 1)
                {
                    return true;
                }

                m_mesh->FindCommonNode(edgeIndex, secondEdgeIndex, edgeEndNodeCache[numNonHangingNodes]);


                if (m_brotherEdges[edgeIndex] == firstEdgeIndex)
                {
                    m_mesh->FindCommonNode(edgeIndex, firstEdgeIndex, hangingNodeCache[numNonHangingNodes]);
                }
                numNonHangingNodes++;
            }
        }

        int numHangingNodes = numEdges - numNonHangingNodes;
        if (numHangingNodes == 0)
        {
            continue;
        }

        // Quads
        if (numNonHangingNodes == numNodesQuads)
        {
            switch (numHangingNodes)
            {
            case 1: // one hanging node
                for (int n = 0; n < numNonHangingNodes; ++n)
                {
                    if (hangingNodeCache[n] < 0)
                    {
                        continue;
                    }

                    auto ee = NextCircularBackwardIndex(n - 1, numNonHangingNodes);
                    auto eee = NextCircularForwardIndex(n, numNonHangingNodes);
                    int newEdgeIndex;
                    successful = m_mesh->ConnectNodes(edgeEndNodeCache[ee], hangingNodeCache[n], newEdgeIndex);
                    successful = successful && m_mesh->ConnectNodes(edgeEndNodeCache[eee], hangingNodeCache[n], newEdgeIndex);
                    if (!successful)
                    {
                        return false;
                    }
                    break;
                }
                break;
            case 2: // two hanging node
                for (int n = 0; n < numNonHangingNodes; ++n)
                {
                    if (hangingNodeCache[n] < 0)
                    {
                        continue;
                    }

                    auto e = NextCircularBackwardIndex(n, numNonHangingNodes);
                    auto ee = NextCircularForwardIndex(n, numNonHangingNodes);
                    auto eee = NextCircularForwardIndex(n + 1, numNonHangingNodes);
                    if (hangingNodeCache[e] >= 0) // left neighbor
                    {
                        int newEdgeIndex;
                        successful = m_mesh->ConnectNodes(hangingNodeCache[e], hangingNodeCache[n], newEdgeIndex);
                        successful = successful && m_mesh->ConnectNodes(hangingNodeCache[n], edgeEndNodeCache[ee], newEdgeIndex);
                        successful = successful && m_mesh->ConnectNodes(edgeEndNodeCache[ee], hangingNodeCache[e], newEdgeIndex);
                        if (!successful)
                        {
                            return false;
                        }
                    }
                    else if (hangingNodeCache[ee] >= 0) // right neighbor
                    {
                        int newEdgeIndex;
                        successful = m_mesh->ConnectNodes(hangingNodeCache[n], hangingNodeCache[ee], newEdgeIndex);
                        successful = successful && m_mesh->ConnectNodes(hangingNodeCache[ee], edgeEndNodeCache[eee], newEdgeIndex);
                        successful = successful && m_mesh->ConnectNodes(edgeEndNodeCache[eee], hangingNodeCache[n], newEdgeIndex);
                        if (!successful)
                        {
                            return false;
                        }
                    }
                    else if (hangingNodeCache[eee] >= 0) // hanging nodes must be opposing
                    {
                        int newEdgeIndex;
                        successful = m_mesh->ConnectNodes(hangingNodeCache[n], hangingNodeCache[eee], newEdgeIndex);
                        if (!successful)
                        {
                            return false;
                        }
                    }
                    break;
                }
                break;
            default: 
                break;
            }
        }
        else if (numNonHangingNodes == 3)
        {
            switch (numHangingNodes)
            {
            case 1: // one hanging node
                for (int n = 0; n < numNonHangingNodes; ++n)
                {
                    if (hangingNodeCache[n] < 0)
                    {
                        continue;
                    }
                    auto e = NextCircularForwardIndex(n, numNonHangingNodes);
                    int newEdgeIndex;
                    successful = m_mesh->ConnectNodes(hangingNodeCache[n], edgeEndNodeCache[e], newEdgeIndex);
                    if (!successful)
                    {
                        return false;
                    }
                    break;
                }
                break;
            case 2: // two hanging node
                for (int n = 0; n < numNonHangingNodes; ++n)
                {
                    if (hangingNodeCache[n] < 0)
                    {
                        continue;
                    }
                    auto e = NextCircularBackwardIndex(n, numNonHangingNodes);
                    auto ee = NextCircularForwardIndex(n, numNonHangingNodes);
                    if (hangingNodeCache[e] >= 0) // left neighbor
                    {
                        int newEdgeIndex;
                        successful = m_mesh->ConnectNodes(hangingNodeCache[n], hangingNodeCache[e], newEdgeIndex);
                        if (!successful)
                        {
                            return false;
                        }
                    }
                    else
                    {
                        int newEdgeIndex;
                        successful = m_mesh->ConnectNodes(hangingNodeCache[n], hangingNodeCache[ee], newEdgeIndex);
                        if (!successful)
                        {
                            return false;
                        }
                    }
                    break;
                }
                break;
            default: 
                break;
            }
        }
        else
        {
            successful = false;
        }
    }
    return successful;
}

bool GridGeom::MeshRefinement::RefineFacesBySplittingEdges(int numEdgesBeforeRefinemet)
{

    //Add new nodes where required
    std::vector<int> notHangingFaceNodes(maximumNumberOfNodesPerFace, intMissingValue);
    std::vector<bool> ishanging(maximumNumberOfNodesPerFace, false);
    std::vector<int> parentEdge(maximumNumberOfNodesPerFace, -1);
    std::vector<Point> facePolygonWithoutHangingNodes(maximumNumberOfNodesPerFace);
    std::vector<Point> middlePointsCache(maximumNumberOfNodesPerFace);
    std::vector<Point> normalsCache(maximumNumberOfNodesPerFace);

    std::vector<int> localEdgesNumFaces(maximumNumberOfEdgesPerFace);

    for (auto e = 0; e < m_mesh->GetNumEdges(); e++)
    {
        if (m_edgeMask[e] == 0)
        {
            continue;
        }

        //Compute the center of the edge
        const auto firstNodeIndex = m_mesh->m_edges[e].first;
        const auto secondNodeIndex = m_mesh->m_edges[e].second;
        const auto firstNode = m_mesh->m_nodes[firstNodeIndex];
        const auto secondNode = m_mesh->m_nodes[secondNodeIndex];

        Point middle{ (firstNode.x + secondNode.x) / 2.0, (firstNode.y + secondNode.y) / 2.0 };
        if (m_mesh->m_projection == Projections::spherical)
        {

            middle.y = (firstNode.y + secondNode.y)/2.0;
            if (std::abs(firstNode.x - secondNode.x) > 180.0)
            {
                middle.x += 180.0;
            }

            // fix at poles
            const auto firstNodeAtPole = IsPointOnPole(firstNode);
            const auto secondNodeAtPole = IsPointOnPole(secondNode);
            if (firstNodeAtPole && !secondNodeAtPole)
            {
                middle.x = secondNode.x;
            }
            else if (!firstNodeAtPole && secondNodeAtPole)
            {
                middle.x = firstNode.x;
            }
        }


        int newNodeIndex;
        m_mesh->InsertNode(middle, newNodeIndex);
        m_edgeMask[e] = newNodeIndex;

        // set mask on the new node
        m_mesh->m_nodeMask[newNodeIndex] = 1;
        if (m_mesh->m_nodeMask[firstNodeIndex] == 0 && m_mesh->m_nodeMask[secondNodeIndex] == 0)
        {
            m_mesh->m_nodeMask[newNodeIndex] = 0;
        }
        else if (m_mesh->m_nodeMask[firstNodeIndex] != 1 || m_mesh->m_nodeMask[secondNodeIndex] != 1)
        {
            m_mesh->m_nodeMask[newNodeIndex] = -1;
        }
        else
        {
            m_mesh->m_nodeMask[newNodeIndex] = 1;
        }
    }

    for (auto f = 0; f < m_mesh->GetNumFaces(); f++)
    {
        if (m_faceMask[f] == 0)
        {
            continue;
        }

        const auto numEdges = m_mesh->GetNumFaceEdges(f);
        // check if the parent face is crossed by the enclosing polygon
        bool isParentCrossed = false;
        for (int e = 0; e < numEdges; ++e)
        {
            const auto n = m_mesh->m_facesNodes[f][e];
            if(m_mesh->m_nodeMask[n]!=1)
            {
                isParentCrossed = true;
                break;
            }
        }

        int numClosedPolygonNodes = 0;
        bool successful = m_mesh->FaceClosedPolygon(f, m_polygonNodesCache, m_localNodeIndexsesCache, m_edgeIndexsesCache, numClosedPolygonNodes);
        if (!successful)
        {
            return false;
        }

        
        int numBrotherEdges = 0;
        int numNonHangingNodes = 0;
        std::fill(notHangingFaceNodes.begin(), notHangingFaceNodes.end(), intMissingValue);
        std::fill(ishanging.begin(), ishanging.end(), false);
        std::fill(parentEdge.begin(), parentEdge.end(), -1);
        for (int e = 0; e < numEdges; e++)
        {
            const auto firstEdge = NextCircularBackwardIndex(e, numEdges);
            const auto secondEdge = NextCircularForwardIndex(e, numEdges);

            int mappedEdge = m_localNodeIndexsesCache[e];
            auto edgeIndex = m_mesh->m_facesEdges[f][mappedEdge];

            mappedEdge = m_localNodeIndexsesCache[firstEdge];
            auto firstEdgeIndex = m_mesh->m_facesEdges[f][mappedEdge];

            mappedEdge = m_localNodeIndexsesCache[secondEdge];
            auto secondEdgeIndex = m_mesh->m_facesEdges[f][mappedEdge];

            if (edgeIndex < 0)
            {
                continue;
            }

            if (m_brotherEdges[edgeIndex] == secondEdgeIndex && secondEdgeIndex >= 0)
            {
                numBrotherEdges++;
                int newNode;
                m_mesh->FindCommonNode(edgeIndex, m_brotherEdges[edgeIndex], newNode);
                notHangingFaceNodes[numNonHangingNodes] = newNode;
                parentEdge[numNonHangingNodes] = edgeIndex;
                numNonHangingNodes++;
            }
            else if (m_brotherEdges[edgeIndex] != firstEdgeIndex || m_brotherEdges[edgeIndex] < 0)
            {
                if (m_edgeMask[edgeIndex] != 0)
                {
                    notHangingFaceNodes[numNonHangingNodes] = m_edgeMask[edgeIndex];
                    parentEdge[numNonHangingNodes] = edgeIndex;
                    numNonHangingNodes++;
                }
            }

            if (numNonHangingNodes >= maximumNumberOfNodesPerFace)
            {
                return true;
            }

            // check if start of this link is hanging
            if (m_brotherEdges[edgeIndex] == firstEdgeIndex && firstEdgeIndex >= 0)
            {
                ishanging[e] = true;
            }
        }

        //compute new center node : circumcenter without hanging nodes for quads, c / g otherwise
        int numNonHangingEdges = 0;
        for (int e = 0; e < numEdges; e++)
        {
            if (!ishanging[e])
            {
                facePolygonWithoutHangingNodes[numNonHangingEdges] = m_polygonNodesCache[e];

                auto mappedEdge = m_localNodeIndexsesCache[e];
                if (mappedEdge > 0)
                {
                    localEdgesNumFaces[numNonHangingEdges] = m_mesh->m_edgesNumFaces[mappedEdge];
                }
                else
                {
                    localEdgesNumFaces[numNonHangingEdges] = 1;
                }
                numNonHangingEdges++;
            }
        }

        // quads
        Point splittingNode(m_mesh->m_facesMassCenters[f]);
        if (numNonHangingEdges == numNodesQuads)
        {

            m_mesh->ComputeFaceCircumenter(facePolygonWithoutHangingNodes,
                middlePointsCache,
                normalsCache,
                numNonHangingEdges,
                localEdgesNumFaces,
                weightCircumCenter,
                splittingNode);

            if (m_mesh->m_projection == Projections::spherical)
            {                
                double miny = std::numeric_limits<double>::max();
                double maxy = std::numeric_limits<double>::min();
                for (int i = 0; i < numNonHangingEdges; ++i)
                {
                    miny = std::min(facePolygonWithoutHangingNodes[i].y, miny);
                    maxy = std::max(facePolygonWithoutHangingNodes[i].y, maxy);
                }

                double middlelatitude;
                middlelatitude = (miny + maxy) / 2.0;
                double ydiff = maxy - miny;
                if (successful && ydiff > 1e-8)
                {
                    splittingNode.y = miny + 2.0*(middlelatitude - miny) / ydiff *(splittingNode.y - miny);
                }
            }
        }

        if (numNonHangingEdges >= numNodesQuads)
        {
            if (numNonHangingNodes > 2)
            {
                int newNodeIndex;
                m_mesh->InsertNode(splittingNode, newNodeIndex);
                for (int n = 0; n < numNonHangingNodes; ++n)
                {
                    int newEdgeIndex;
                    m_mesh->ConnectNodes(notHangingFaceNodes[n], newNodeIndex, newEdgeIndex);
                }

                m_mesh->m_nodeMask[newNodeIndex] = 1;
                if (isParentCrossed)
                {
                    //deactive nodes in cells crossed by polygon
                    m_mesh->m_nodeMask[newNodeIndex] = -1;
                }
            }
            else if (numNonHangingNodes == 2)
            {
                int newEdgeIndex;
                m_mesh->ConnectNodes(notHangingFaceNodes[0], notHangingFaceNodes[1], newEdgeIndex);
            }
        }
        else
        {
            for (int n = 0; n < numNonHangingNodes; ++n)
            {
                auto nn = NextCircularForwardIndex(n, numNonHangingNodes);
                int newEdgeIndex;
                m_mesh->ConnectNodes(notHangingFaceNodes[n], notHangingFaceNodes[nn], newEdgeIndex);
            }
        }
    }

    //Split original edges
    for (int e = 0; e < numEdgesBeforeRefinemet; ++e)
    {
        if (m_edgeMask[e] > 0)
        {
            int newEdgeIndex;
            m_mesh->ConnectNodes(m_edgeMask[e], m_mesh->m_edges[e].second, newEdgeIndex);
            m_mesh->m_edges[e].second = m_edgeMask[e];
            ResizeVectorIfNeeded(m_mesh->GetNumEdges(), m_brotherEdges);
            m_brotherEdges[newEdgeIndex] = e;
            m_brotherEdges[e] = newEdgeIndex;
        }
    }

    return true;
}

bool GridGeom::MeshRefinement::ComputeNodeMaskAtPolygonPerimeter()
{
    for (int f = 0; f < m_mesh->GetNumFaces(); f++)
    {
        bool crossing = false;
        const auto numnodes = m_mesh->GetNumFaceEdges(f);
        for (int n = 0; n < numnodes; n++)
        {
            int nodeIndex = m_mesh->m_facesNodes[f][n];

            if (m_mesh->m_nodeMask[nodeIndex] == 0)
            {
                crossing = true;
                break;
            }
        }

        if (crossing)
        {
            m_faceMask[f] = 0;
            for (int n = 0; n < numnodes; n++)
            {
                int nodeIndex = m_mesh->m_facesNodes[f][n];
                if (m_mesh->m_nodeMask[nodeIndex] == 1)
                {
                    m_mesh->m_nodeMask[nodeIndex] = -2;
                }
            }
        }
    }
    return true;
}

bool GridGeom::MeshRefinement::ComputeRefinementMasksFromSamples(std::vector<Sample>& samples)
{
    std::fill(m_edgeMask.begin(), m_edgeMask.end(), 0);
    std::fill(m_faceMask.begin(), m_faceMask.end(), 0);
    m_polygonNodesCache.resize(maximumNumberOfNodesPerFace + 1);
    m_localNodeIndexsesCache.resize(maximumNumberOfNodesPerFace + 1, intMissingValue);
    m_edgeIndexsesCache.resize(maximumNumberOfEdgesPerFace + 1, intMissingValue);
    std::vector<int> refineEdgeCache(maximumNumberOfEdgesPerFace);
    std::fill(m_subtractedSample.begin(), m_subtractedSample.end(), false);

    for (int f = 0; f < m_mesh->GetNumFaces(); f++)
    {
        int numClosedPolygonNodes = 0;
        m_mesh->FaceClosedPolygon(f, m_polygonNodesCache, m_localNodeIndexsesCache, m_edgeIndexsesCache, numClosedPolygonNodes);

        int numHangingEdges;
        int numHangingNodes;
        int numEdgesToRefine;
        bool successful = FindHangingNodes(f, numHangingEdges, numHangingNodes, numEdgesToRefine);
        if (!successful)
        {
            return false;
        }

        std::fill(refineEdgeCache.begin(), refineEdgeCache.end(), 0);
        int numEdgesToBeRefined = 0;
        successful = ComputeEdgesRefinementMaskFromSamples(m_mesh->GetNumFaceEdges(f), samples, refineEdgeCache, numEdgesToBeRefined);
        if (!successful)
        {
            return false;
        }

        m_faceMask[f] = 0;
        if (numEdgesToBeRefined > 1)
        {
            m_faceMask[f] = 1;

            for (int n = 0; n < m_mesh->GetNumFaceEdges(f); n++)
            {
                if (refineEdgeCache[n] == 1)
                {
                    int node = m_localNodeIndexsesCache[n];
                    if (node < 0)
                    {
                        continue;
                    }
                    int edgeIndex = m_mesh->m_facesEdges[f][node];
                    if (edgeIndex >= 0)
                    {
                        m_edgeMask[edgeIndex] = 1;
                    }
                }
            }
        }
    }

    return true;
};

bool GridGeom::MeshRefinement::FindHangingNodes(int faceIndex,
    int& numHangingEdges,
    int& numHangingNodes,
    int& numEdgesToRefine)
{

    numEdgesToRefine = 0;
    numHangingEdges = 0;
    numHangingNodes = 0;
    auto numFaceNodes = m_mesh->GetNumFaceEdges(faceIndex);

    if (numFaceNodes > maximumNumberOfEdgesPerNode)
    {
        return true;
    }

    m_isHangingNodeCache.resize(maximumNumberOfNodesPerFace);
    m_isHangingEdgeCache.resize(maximumNumberOfEdgesPerFace);
    std::fill(m_isHangingNodeCache.begin(), m_isHangingNodeCache.end(), false);
    std::fill(m_isHangingEdgeCache.begin(), m_isHangingEdgeCache.end(), false);

    int kknod = -1;

    for (int n = 0; n < numFaceNodes; n++)
    {
        auto edgeIndex = m_mesh->m_facesEdges[faceIndex][n];
        if (m_edgeMask[edgeIndex] != 0)
        {
            numEdgesToRefine += 1;
        }

        // check if the parent edge is in the cell
        if (m_brotherEdges[edgeIndex] != intMissingValue)
        {
            const auto e = NextCircularBackwardIndex(n, numFaceNodes);
            const auto ee = NextCircularForwardIndex(n, numFaceNodes);
            const auto firstEdgeIndex = m_mesh->m_facesEdges[faceIndex][e];
            const auto secondEdgeIndex = m_mesh->m_facesEdges[faceIndex][ee];

            int commonNode = intMissingValue;
            if (m_brotherEdges[edgeIndex] == firstEdgeIndex)
            {
                m_mesh->FindCommonNode(edgeIndex, firstEdgeIndex, commonNode);
            }
            else if (m_brotherEdges[edgeIndex] == secondEdgeIndex)
            {
                m_mesh->FindCommonNode(edgeIndex, secondEdgeIndex, commonNode);
            }

            if (commonNode != intMissingValue)
            {
                m_isHangingEdgeCache[n] = true;
                numHangingEdges++;
                for (int nn = 0; nn < numFaceNodes; nn++)
                {
                    kknod = NextCircularForwardIndex(kknod, numFaceNodes);

                    if (m_mesh->m_facesNodes[faceIndex][kknod] == commonNode && !m_isHangingNodeCache[kknod])
                    {
                        numHangingNodes++;
                        m_isHangingNodeCache[kknod] = true;
                        break;
                    }
                }
            }
        }
    }

    return true;
}

bool GridGeom::MeshRefinement::ComputeEdgesRefinementMaskFromSamples( int numPolygonNodes,
                                                                      std::vector<Sample>& samples,
                                                                      std::vector<int>& refineEdgeCache,
                                                                      int& numEdgesToBeRefined )
{
    numEdgesToBeRefined = 0;
    // Not implemented yet
    if (m_refinementType == RefinementType::RidgeRefinement)
    {
        return true;
    }

    // compute all lengths
    m_polygonEdgesLengthsCache.resize(numPolygonNodes);
    for (int i = 0; i < numPolygonNodes; i++)
    {
        int edgeIndex = m_edgeIndexsesCache[i];
        m_polygonEdgesLengthsCache[i] = m_mesh->m_edgeLengths[edgeIndex];
    }
    
    //find center of mass
    Point centerOfMass;
    double area;
    bool successful = FaceAreaAndCenterOfMass(m_polygonNodesCache, numPolygonNodes, m_mesh->m_projection, area, centerOfMass);
    if (!successful)
    {
        return false;
    }

    // a default value
    double refinementValue = 0.0;
    if (m_refinementType == RefinementType::RefinementLevels)
    {
        refinementValue = ComputeFaceRefinementFromSamples(numPolygonNodes, samples, Max, centerOfMass);
        // nothing to do 
        if (refinementValue <= 0) 
        {
            return true;
        }
        for (int i = 0; i < m_samplesRTree.GetQueryResultSize(); i++)
        {
            // decrease the sample values in the current cells by one
            const auto sampleIndex = m_samplesRTree.GetQuerySampleIndex(i);
            if (!m_subtractedSample[sampleIndex])
            {
                samples[sampleIndex].value -= 1.0;
                m_subtractedSample[sampleIndex] = true;
            }
        }
    }

    if (m_refinementType == RefinementType::WaveCourant)
    {
        refinementValue = ComputeFaceRefinementFromSamples(numPolygonNodes, samples, KdTree, centerOfMass);
    }

    if (refinementValue == doubleMissingValue && m_refineOutsideFace)
    {
        // find nearest face
        m_samplesRTree.NearestNeighbour(centerOfMass);
        if (m_samplesRTree.GetQueryResultSize() > 0)
        {
            auto sampleIndex = m_samplesRTree.GetQuerySampleIndex(0);
            if (sampleIndex >= 0)
            {
                refinementValue = samples[sampleIndex].value;
            }
        }
    }

    if (refinementValue == doubleMissingValue)
    {
        return true;
    }

    for (int i = 0; i < numPolygonNodes; i++)
    {
        if (m_polygonEdgesLengthsCache[i] < mergingDistance)
        {
            numEdgesToBeRefined++;
            continue;
        }
        
        bool doRefinement = false;

        // based on wave courant
        if (m_refinementType == RefinementType::WaveCourant)
        {
            double newEdgeLength = 0.5 * m_polygonEdgesLengthsCache[i];
            double c = std::sqrt(gravity * std::abs(refinementValue));
            double waveCourant = c * m_deltaTimeMaxCourant / m_polygonEdgesLengthsCache[i];    
            doRefinement = waveCourant < 1.0 && std::abs(newEdgeLength - m_minimumFaceSize) < std::abs(m_polygonEdgesLengthsCache[i] - m_minimumFaceSize);
        }

        // based on refinement levels
        if (m_refinementType == RefinementType::RefinementLevels && refinementValue > 0.0)
        {
            doRefinement = true;
        }

        if (doRefinement)
        {
            numEdgesToBeRefined++;
            refineEdgeCache[i] = 1;
        }
    }


    if (numEdgesToBeRefined > 0)
    {
        numEdgesToBeRefined = 0;
        for (int i = 0; i < numPolygonNodes; i++)
        {
            if (refineEdgeCache[i] == 1 || m_isHangingNodeCache[i])
            {
                numEdgesToBeRefined++;
            }
        }
    }

    if (!m_directionalRefinement)
    {
        if (numEdgesToBeRefined == numPolygonNodes)
        {
            for (int i = 0; i < numPolygonNodes; i++)
            {
                if (!m_isHangingNodeCache[i])
                {
                    refineEdgeCache[i] = 1;
                }
            }
        }
        else
        {
            numEdgesToBeRefined = 0;
        }
    }

    return true;
}

double GridGeom::MeshRefinement::ComputeFaceRefinementFromSamples(int numPolygonNodes, const std::vector<Sample>& samples, AveragingMethod averagingMethod, Point centerOfMass)
{
    double refinementValue = 0.0;
    bool success = Averaging(samples, numPolygonNodes, m_polygonNodesCache, centerOfMass, m_mesh->m_projection, m_samplesRTree, averagingMethod, refinementValue);
    if (!success)
    {
        return doubleMissingValue;
    }

    return refinementValue;
}

bool  GridGeom::MeshRefinement::ComputeEdgesRefinementMask()
{
    bool repeat = true;
    int iter = 0;
    const int numMaxIterations = 6;
    std::vector<int> isQuadEdge(numNodesQuads);
    std::vector<int> numOfEdges(maximumNumberOfEdgesPerFace);

    while (repeat && iter < numMaxIterations)
    {
        repeat = false;
        iter++;

        for (int f = 0; f < m_mesh->GetNumFaces(); f++)
        {
            if (m_faceMask[f] == 0)
            {
                continue;
            }

            int numHangingEdges;
            int numHangingNodes;
            int numEdgesToRefine;
            bool successful = FindHangingNodes(f, numHangingEdges, numHangingNodes, numEdgesToRefine);
            if (!successful)
            {
                return false;
            }

            auto numFaceNodes = m_mesh->GetNumFaceEdges(f);

            // non-quads
            int numNodesEffective = numFaceNodes - numHangingNodes;
            if (numNodesEffective != numNodesQuads)
            {
                for (int n = 0; n < numFaceNodes; n++)
                {
                    const auto e = NextCircularBackwardIndex(n, numFaceNodes);
                    const auto ee = NextCircularForwardIndex(n, numFaceNodes);

                    auto edgeIndex = m_mesh->m_facesEdges[f][n];

                    auto firstEdgeIndex = m_mesh->m_facesEdges[f][e];
                    auto secondEdgeIndex = m_mesh->m_facesEdges[f][ee];

                    //do not refine edges with an hanging node
                    if (m_brotherEdges[edgeIndex] != firstEdgeIndex && m_brotherEdges[edgeIndex] != secondEdgeIndex)
                    {
                        m_edgeMask[edgeIndex] = 1;
                    }
                }
            }
            if (numNodesEffective == numNodesQuads)
            {
                // number the links in the cell, links that share a hanging node will have the same number
                int num = 0;
                for (int n = 0; n < numFaceNodes; n++)
                {
                    auto edgeIndex = m_mesh->m_facesEdges[f][n];
                    numOfEdges[n] = num;

                    if (m_edgeMask[edgeIndex] != 0)
                    {
                        isQuadEdge[num] = m_edgeMask[edgeIndex];
                    }

                    const auto ee = NextCircularForwardIndex(n, numFaceNodes);

                    const auto secondEdgeIndex = m_mesh->m_facesEdges[f][ee];

                    if (n != numFaceNodes - 1 && m_brotherEdges[edgeIndex] != secondEdgeIndex)
                    {
                        num++;
                    }
                    if (m_brotherEdges[edgeIndex] == secondEdgeIndex)
                    {
                        isQuadEdge[num] = 1;
                    }
                }

                if (num + 1 != numNodesQuads)
                {
                    return false;
                }

                int numEdgesToRefine = 0;
                int firstEdgeIndex = 0;
                int secondEdgeIndex = 0;
                for (int i = 0; i < numNodesQuads; i++)
                {
                    if (isQuadEdge[i] != 0)
                    {
                        numEdgesToRefine++;
                        if (firstEdgeIndex == 0)
                        {
                            firstEdgeIndex = i;
                        }
                        else if (secondEdgeIndex == 0)
                        {
                            secondEdgeIndex = i;
                        }
                    }
                }

                const auto edgeIndexDifference = secondEdgeIndex - firstEdgeIndex;
                bool refineAllEdges = false;
                if (numEdgesToRefine == 2 && (edgeIndexDifference == 1 || edgeIndexDifference == 3))
                {
                    repeat = true;
                    refineAllEdges = true;
                }

                for (int n = 0; n < numFaceNodes; n++)
                {

                    auto edgeIndex = m_mesh->m_facesEdges[f][n];
                    if (m_edgeMask[edgeIndex] > 0)
                    {
                        continue;
                    }

                    if (refineAllEdges != true && m_edgeMask[edgeIndex] != -1)
                    {
                        continue;
                    }

                    const auto e = NextCircularBackwardIndex(n, numFaceNodes);
                    const auto ee = NextCircularForwardIndex(n, numFaceNodes);

                    auto secondEdgeIndex = m_mesh->m_facesEdges[f][ee];

                    if (numOfEdges[n] != numOfEdges[e] && numOfEdges[n] != numOfEdges[ee])
                    {
                        m_edgeMask[edgeIndex] = 1;
                    }

                }
            }
        }
    }

    if (repeat)
    {
        // solution did not converge
        return false;
    }

    // only keep m_edgeMask = 1, set other values to 0
    for (int i = 0; i < m_edgeMask.size(); i++)
    {
        if (m_edgeMask[i] != 1)
        {
            m_edgeMask[i] = 0;
        }
    }

    return true;
}

bool  GridGeom::MeshRefinement::ComputeIfFaceShouldBeSplitted()
{
    const int maxiter = 1000;
    int num = 1;
    int iter = 0;
    while (num != 0)
    {
        iter++;
        if (iter > maxiter)
        {
            break;
        }

        num = 0;
        for (int f = 0; f < m_mesh->GetNumFaces(); f++)
        {
            if (m_faceMask[f] != 0 && m_faceMask[f] != -1)
            {
                continue;
            }
            int numHangingEdges;
            int numHangingNodes;
            int numEdgesToRefine;

            bool successful = FindHangingNodes(f, numHangingEdges, numHangingNodes, numEdgesToRefine);
            if(!successful)
            {
                return false;
            }

            bool isSplittingRequired = false;

            // check if the edge has a brother edge and needs to be refined
            auto numFaceNodes = m_mesh->GetNumFaceEdges(f);

            if (numFaceNodes > maximumNumberOfEdgesPerFace)
            {
                return true;
            }

            for (int n = 0; n < numFaceNodes; n++)
            {
                int edgeIndex = m_mesh->m_facesEdges[f][n];
                if (m_isHangingEdgeCache[n] && m_edgeMask[edgeIndex] > 0)
                {
                    isSplittingRequired = true;
                }
            }

            //compute the effective face type
            int numNodesEffective = numFaceNodes - numHangingEdges / 2;
            if (2 * (numFaceNodes - numNodesEffective) != numHangingEdges)
            {
                //uneven number of brotherlinks   
                // TODO: ADD DOT
            }

            if (numFaceNodes + numEdgesToRefine > maximumNumberOfEdgesPerFace ||          // would result in unsupported cells after refinement
                numFaceNodes - numHangingNodes - numEdgesToRefine <= 1 ||                 // faces with only one unrefined edge
                numNodesEffective == numEdgesToRefine)                                    // refine all edges
            {
                isSplittingRequired = true;
            }

            if (isSplittingRequired)
            {
                if (m_faceMask[f] != -1)
                {
                    m_faceMask[f] = 2;
                }
                else
                {
                    m_faceMask[f] = -2;
                }

                for (int n = 0; n < numFaceNodes; n++)
                {
                    int edgeIndex = m_mesh->m_facesEdges[f][n];
                    if (!m_isHangingEdgeCache[n] && m_edgeMask[edgeIndex] == 0)
                    {
                        m_edgeMask[edgeIndex] = 1;
                        num++;
                    }
                    if (iter == maxiter)
                    {
                        //TODO: ADD DOT/MESSAGES
                    }
                }

            }
        }
    }

    return true;
}


bool GridGeom::MeshRefinement::FindBrotherEdges()
{
    m_brotherEdges.resize(m_mesh->GetNumEdges());
    std::fill(m_brotherEdges.begin(), m_brotherEdges.end(), intMissingValue);

    for (int n = 0; n < m_mesh->GetNumNodes(); n++)
    {
        const auto numEdgesNodes = m_mesh->m_nodesNumEdges[n];
        for (int e = 0; e < numEdgesNodes; e++)
        {

            const auto firstEdgeIndex = m_mesh->m_nodesEdges[n][e];
            if (m_mesh->GetNumEdgesFaces(firstEdgeIndex) < 1)
            {
                continue;
            }

            const auto ee = NextCircularForwardIndex(e, numEdgesNodes);
            const auto secondEdgeIndex = m_mesh->m_nodesEdges[n][ee];
            if (m_mesh->GetNumEdgesFaces(secondEdgeIndex) < 1)
            {
                continue;
            }

            // both edges should share the same face
            const auto firstEdgeLeftFace = m_mesh->m_edgesFaces[firstEdgeIndex][0];
            const auto firstEdgeRighFace = m_mesh->GetNumEdgesFaces(firstEdgeIndex) == 1 ? firstEdgeLeftFace : m_mesh->m_edgesFaces[firstEdgeIndex][1];
            const auto secondEdgeLeftFace = m_mesh->m_edgesFaces[secondEdgeIndex][0];
            const auto secondEdgeRighFace = m_mesh->GetNumEdgesFaces(secondEdgeIndex) == 1 ? secondEdgeLeftFace : m_mesh->m_edgesFaces[secondEdgeIndex][1];

            if (firstEdgeLeftFace != secondEdgeLeftFace &&
                firstEdgeLeftFace != secondEdgeRighFace &&
                firstEdgeRighFace != secondEdgeLeftFace &&
                firstEdgeRighFace != secondEdgeRighFace)
            {
                continue;
            }

            //check if node k is in the middle
            const auto firstEdgeOtherNode = m_mesh->m_edges[firstEdgeIndex].first + m_mesh->m_edges[firstEdgeIndex].second - n;
            const auto secondEdgeOtherNode = m_mesh->m_edges[secondEdgeIndex].first + m_mesh->m_edges[secondEdgeIndex].second - n;
            
            //compute tolerance
            const auto firstEdgeSquaredLength = ComputeSquaredDistance(m_mesh->m_nodes[firstEdgeOtherNode], m_mesh->m_nodes[n], m_mesh->m_projection);
            const auto secondEdgeSquaredLength = ComputeSquaredDistance(m_mesh->m_nodes[secondEdgeOtherNode], m_mesh->m_nodes[n], m_mesh->m_projection);
            const auto squaredTolerance = 0.0000001 * std::max(firstEdgeSquaredLength, secondEdgeSquaredLength);

            //The center of the two edges coincides with the shared node
            Point centre;
            ComputeMiddlePoint(m_mesh->m_nodes[firstEdgeOtherNode], m_mesh->m_nodes[secondEdgeOtherNode], m_mesh->m_projection, centre);
            const auto squaredDistanceFromCentre = ComputeSquaredDistance(centre, m_mesh->m_nodes[n], m_mesh->m_projection);
            if (squaredDistanceFromCentre < squaredTolerance)
            {
                m_brotherEdges[firstEdgeIndex] = secondEdgeIndex;
                m_brotherEdges[secondEdgeIndex] = firstEdgeIndex;
            }
        }
    }

    return true;
}

bool GridGeom::MeshRefinement::SmoothEdgeRefinementMask()
{
    return true;
}
