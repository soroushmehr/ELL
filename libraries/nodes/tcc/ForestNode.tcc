////////////////////////////////////////////////////////////////////////////////////////////////////
//
//  Project:  Embedded Machine Learning Library (EMLL)
//  File:     ForestNode.cpp (nodes)
//  Authors:  Ofer Dekel
//
////////////////////////////////////////////////////////////////////////////////////////////////////

#include "ForestNode.h"
#include "ConstantNode.h"
#include "ElementSelectorNode.h"
#include "BinaryOperationNode.h"
#include "MultiplexorNode.h"
#include "SingleElementThresholdNode.h"
#include "SumNode.h"

// stl
#include <vector>
#include <memory>

namespace nodes
{
    template<typename SplitRuleType, typename EdgePredictorType>
    ForestNode<SplitRuleType, EdgePredictorType>::ForestNode(const model::PortElements<double>& input, const predictors::ForestPredictor<SplitRuleType, EdgePredictorType>& forest) : Node({ &_input }, { &_output, &_treeOutputs, &_edgeIndicatorVector }), _input(this, input, inputPortName), _output(this, outputPortName, 1), _treeOutputs(this, treeOutputsPortName, forest.NumTrees()), _edgeIndicatorVector(this, edgeIndicatorVectorPortName, forest.NumEdges()), _forest(forest)
    {}

    template<typename SplitRuleType, typename EdgePredictorType>
    void ForestNode<SplitRuleType, EdgePredictorType>::Copy(model::ModelTransformer& transformer) const
    {
        auto newPortElements = transformer.TransformPortElements(_input.GetPortElements());
        auto newNode = transformer.AddNode<ForestNode<SplitRuleType, EdgePredictorType>>(newPortElements, _forest);
        transformer.MapNodeOutput(output, newNode->output);
        transformer.MapNodeOutput(treeOutputs, newNode->treeOutputs);
        transformer.MapNodeOutput(edgeIndicatorVector, newNode->edgeIndicatorVector);
    }

    template<typename SplitRuleType, typename EdgePredictorType>
    void ForestNode<SplitRuleType, EdgePredictorType>::RefineNode(model::ModelTransformer & transformer) const
    {        
        auto newPortElements = transformer.TransformPortElements(_input.GetPortElements());      
        const auto& interiorNodes = _forest.GetInteriorNodes();

        // create a place to store references to the output ports of the sub-models at each interior node
        std::vector<model::PortElements<bool>> interiorNodeSplitIndicators(interiorNodes.size());
        std::vector<model::PortElements<double>> interiorNodeSubModels(interiorNodes.size());
        
        // visit interior nodes bottom-up (in reverse topological order)
        for(int nodeIndex = (int)interiorNodes.size()-1; nodeIndex >= 0; --nodeIndex) // Note: index var must be signed or else end condition is never met
        {            
            const auto& edges = interiorNodes[nodeIndex].GetOutgoingEdges();

            // get the sub-model that represents each outgoing edge
            model::PortElements<double> edgeOutputs;
            for(size_t j = 0; j < edges.size(); ++j)
            {
                const auto& edgePredictor = edges[j].GetPredictor();
                auto edgePredictorNode = AddNodeToModelTransformer(newPortElements, edgePredictor, transformer);

                if(edges[j].IsTargetInterior()) // target node is itself an interior node: reverse topological order guarantees that it's already visited
                {
                    model::PortElements<double> elements = interiorNodeSubModels[edges[j].GetTargetNodeIndex()];

                    auto sumNode = transformer.AddNode<BinaryOperationNode<double>>(edgePredictorNode->output, elements, BinaryOperationNode<double>::OperationType::add);
                    edgeOutputs.Append(sumNode->output);
                }
                else // target node is a leaf
                {
                    edgeOutputs.Append(edgePredictorNode->output);
                }
            }

            // add the sub-model that computes the split rule
            auto splitRuleNode = AddNodeToModelTransformer(newPortElements, interiorNodes[nodeIndex].GetSplitRule(), transformer);
            interiorNodeSplitIndicators[nodeIndex] = {splitRuleNode->output};
            
            // ...and selects the output value
            auto selectorNode = transformer.AddNode<ElementSelectorNode<double, bool>>(edgeOutputs, splitRuleNode->output);
            interiorNodeSubModels[nodeIndex] = {selectorNode->output};
        }

        // Now compute the edge indicator vector
        std::vector<model::PortElements<bool>> edgeIndicatorSubModels(_forest.NumEdges());

        // Vector with index of the incoming edge for each internal node (with sentinel value of -1 for tree roots)
        std::vector<int> incomingEdgeIndices(interiorNodes.size(), -1);
        for(size_t nodeIndex = 0; nodeIndex < interiorNodes.size(); ++nodeIndex)
        {
            auto parentEdgeIndex = incomingEdgeIndices[nodeIndex];
            auto isRoot = parentEdgeIndex == -1;
            const auto& node = interiorNodes[nodeIndex];
            const auto& edgeSelector = interiorNodeSplitIndicators[nodeIndex];

            model::PortElements<bool> child1Out;
            model::PortElements<bool> child2Out;
            
            // multiplexor
            if(isRoot)
            {
                auto notNode = transformer.AddNode<UnaryOperationNode<bool>>(edgeSelector, UnaryOperationNode<bool>::OperationType::logicalNot);
                child1Out = {notNode->output};
                child2Out = {edgeSelector};
            }
            else
            {
               auto parentIndicator = edgeIndicatorSubModels[parentEdgeIndex];
               auto muxNode = transformer.AddNode<MultiplexorNode<bool, bool>>(parentIndicator, edgeSelector, 2);                
               child1Out = {muxNode->output, 0};
               child2Out = {muxNode->output, 1};
            }

            auto firstEdgeIndex = node.GetFirstEdgeIndex();
            edgeIndicatorSubModels[firstEdgeIndex] = child1Out;
            edgeIndicatorSubModels[firstEdgeIndex+1] = child2Out;

            const auto& childEdges = node.GetOutgoingEdges();
            for(size_t edgePosition = 0; edgePosition < childEdges.size(); ++edgePosition)
            {
                // If this edge's target node has an outgoing edge, record ourself as its parent
                if(childEdges[edgePosition].IsTargetInterior())
                {
                    auto childNode = childEdges[edgePosition].GetTargetNodeIndex();
                    incomingEdgeIndices[childNode] = static_cast<int>(firstEdgeIndex+edgePosition);
                }
            }
        }
        // collect the entries for the indicator vector
        model::PortElements<bool> edgeIndicatorVectorElements(edgeIndicatorSubModels);

        // collect the sub-models that represent the trees of the forest
        model::PortElements<double> treeSubModels;
        for(size_t rootIndex : _forest.GetRootIndices())
        {
            treeSubModels.Append(interiorNodeSubModels[rootIndex]);
        }

        // add the bias term, but first make a copy for individual tree outputs
        auto individualTreeOutputs = treeSubModels;
        auto biasNode = transformer.AddNode<ConstantNode<double>>(_forest.GetBias());
        treeSubModels.Append(biasNode->output);

        // sum all of the trees
        auto sumNode = transformer.AddNode<SumNode<double>>(treeSubModels);

        // Map all the outputs from the original node to the refined graph outputs         
        transformer.MapNodeOutput(output, sumNode->output);
        transformer.MapNodeOutput(treeOutputs, individualTreeOutputs);
        transformer.MapNodeOutput(edgeIndicatorVector, edgeIndicatorVectorElements);
    }

    template<typename SplitRuleType, typename EdgePredictorType>
    void ForestNode<SplitRuleType, EdgePredictorType>::Compute() const
    {
        // forest output
        _output.SetOutput({ _forest.Predict(_input) });

        // individual tree outputs
        std::vector<double> treeOutputs(_forest.NumTrees());
        for(size_t i=0; i<_forest.NumTrees(); ++i)
        {
            treeOutputs[i] = _forest.Predict(_input, _forest.GetRootIndex(i));
        }
        _treeOutputs.SetOutput(std::move(treeOutputs));

        // path indicator
        auto edgeIndicator = _forest.GetEdgeIndicatorVector(_input);
        _edgeIndicatorVector.SetOutput(std::move(edgeIndicator));
    }
}