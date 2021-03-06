/* This file is part of:
 * Operon - Large Scale Genetic Programming Framework
 *
 * Licensed under the ISC License <https://opensource.org/licenses/ISC> 
 * Copyright (C) 2019 Bogdan Burlacu 
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE. 
 */

#include "core/dataset.hpp"
#include "core/eval.hpp"
#include "core/format.hpp"
#include "core/grammar.hpp"
#include "core/stats.hpp"
#include "operators/creator.hpp"
#include "operators/crossover.hpp"
#include <algorithm>
#include <catch2/catch.hpp>
#include <execution>

namespace Operon::Test {
TEST_CASE("Sample nodes from grammar", "[implementation]")
{
    Grammar grammar;
    grammar.SetConfig(Grammar::Arithmetic | NodeType::Log | NodeType::Exp);
    grammar.Enable(NodeType::Add, 2);
    Operon::Random rd(std::random_device {}());

    std::vector<double> observed(NodeTypes::Count, 0);
    size_t r = grammar.EnabledSymbols().size() + 1;

    const size_t nTrials = 1'000'000;
    for (auto i = 0u; i < nTrials; ++i) {
        auto node = grammar.SampleRandomSymbol(rd, 0, 2);
        ++observed[NodeTypes::GetIndex(node.Type)];
    }
    std::transform(std::execution::unseq, observed.begin(), observed.end(), observed.begin(), [&](double v) { return v / nTrials; });
    std::vector<double> actual(NodeTypes::Count, 0);
    for (size_t i = 0; i < observed.size(); ++i) {
        auto nodeType = static_cast<NodeType>(1u << i);
        actual[NodeTypes::GetIndex(nodeType)] = grammar.GetFrequency(nodeType);
    }
    auto freqSum = std::reduce(std::execution::unseq, actual.begin(), actual.end(), 0.0, std::plus {});
    std::transform(std::execution::unseq, actual.begin(), actual.end(), actual.begin(), [&](double v) { return v / freqSum; });
    auto chi = 0.0;
    for (auto i = 0u; i < observed.size(); ++i) {
        auto nodeType = static_cast<NodeType>(1u << i);
        if (!grammar.IsEnabled(nodeType))
            continue;
        auto x = observed[i];
        auto y = actual[i];
        fmt::print("{:>8} observed {:.4f}, expected {:.4f}\n", Node(nodeType).Name(), x, y);
        chi += (x - y) * (x - y) / y;
    }
    chi *= nTrials;

    auto criticalValue = r + 2 * std::sqrt(r);
    fmt::print("chi = {}, critical value = {}\n", chi, criticalValue);
    REQUIRE(chi <= criticalValue);
}

TEST_CASE("Tree shape", "[implementation]")
{
    auto target = "Y";
    auto ds = Dataset("../data/Poly-10.csv", true);
    auto variables = ds.Variables();
    std::vector<Variable> inputs;
    std::copy_if(variables.begin(), variables.end(), std::back_inserter(inputs), [&](auto& v) { return v.Name != target; });
    size_t maxDepth = 1000,
           maxLength = 100;
    auto sizeDistribution = std::uniform_int_distribution<size_t>(maxLength, maxLength);
    auto creator = BalancedTreeCreator(sizeDistribution, maxDepth, maxLength);

    Grammar grammar;
    grammar.SetConfig(Grammar::Arithmetic | NodeType::Log | NodeType::Exp);
    grammar.Enable(NodeType::Add, 1);
    grammar.Enable(NodeType::Mul, 1);
    grammar.Enable(NodeType::Sub, 1);
    grammar.Enable(NodeType::Div, 1);
    Operon::Random random(1234);

    auto tree = creator(random, grammar, inputs);
    fmt::print("Tree length: {}\n", tree.Length());
    fmt::print("{}\n", TreeFormatter::Format(tree, ds));
}

TEST_CASE("Tree initialization (balanced)", "[implementation]")
{
    auto target = "Y";
    auto ds = Dataset("../data/Poly-10.csv", true);
    auto variables = ds.Variables();
    std::vector<Variable> inputs;
    std::copy_if(variables.begin(), variables.end(), std::back_inserter(inputs), [&](auto& v) { return v.Name != target; });

    size_t maxDepth = 1000, 
           minLength = 100,
           maxLength = 100;

    const size_t nTrees = 1'000'000;

    auto sizeDistribution = std::uniform_int_distribution<size_t>(minLength, maxLength);
    //auto sizeDistribution = std::normal_distribution<Operon::Scalar> { maxLength / 2.0, 10 };
    auto creator = BalancedTreeCreator(sizeDistribution, maxDepth, maxLength);
    Grammar grammar;
    grammar.SetConfig(NodeType::Add | NodeType::Exp | NodeType::Variable);
    //Operon::Random rd(std::random_device {}());
    Operon::Random random(1234);

    auto trees = std::vector<Tree>(nTrees);
    std::generate(trees.begin(), trees.end(), [&]() { return creator(random, grammar, inputs); });

    auto totalLength = std::transform_reduce(std::execution::par_unseq, trees.begin(), trees.end(), 0.0, std::plus<size_t> {}, [](const auto& tree) { return tree.Length(); });
    auto totalShape = std::transform_reduce(std::execution::par_unseq, trees.begin(), trees.end(), 0.0, std::plus<size_t> {}, [](const auto& tree) { return tree.VisitationLength(); });
    fmt::print("Balanced tree creator - length({},{}) = {}\n", maxDepth, maxLength, totalLength / trees.size());
    fmt::print("Balanced tree creator - shape({},{}) = {}\n", maxDepth, maxLength, totalShape / trees.size());

    SECTION("Symbol frequencies")
    {
        std::array<size_t, NodeTypes::Count> symbolFrequencies;
        symbolFrequencies.fill(0u);
        for (const auto& tree : trees) {
            for (const auto& node : tree.Nodes()) {
                symbolFrequencies[NodeTypes::GetIndex(node.Type)]++;
            }
        }
        fmt::print("Symbol frequencies: \n");

        for (size_t i = 0; i < symbolFrequencies.size(); ++i) {
            auto node = Node(static_cast<NodeType>(1u << i));
            if (!grammar.IsEnabled(node.Type))
                continue;
            fmt::print("{}\t{:.3f} %\n", node.Name(), symbolFrequencies[i] / totalLength);
        }
    }

    SECTION("Symbol frequency vs tree length")
    {
        Eigen::MatrixXd counts = Eigen::MatrixXd::Zero(maxLength, NodeTypes::Count);
        std::cout << "\t";
        for (size_t i = 0; i < NodeTypes::Count; ++i) {
            std::cout << Node(static_cast<NodeType>(1u << i)).Name() << "\t";
        }
        std::cout << "\n";

        for (const auto& tree : trees) {
            for (const auto& node : tree.Nodes()) {
                Expects(grammar.IsEnabled(node.Type));
                counts(tree.Length() - 1, NodeTypes::GetIndex(node.Type))++;
            }
        }
        std::cout << counts << "\n";
    }

    SECTION("Variable frequencies")
    {
        fmt::print("Variable frequencies:\n");
        size_t totalVars = 0;
        std::vector<size_t> variableFrequencies(inputs.size());
        for (const auto& t : trees) {
            for (const auto& node : t.Nodes()) {
                if (node.IsVariable()) {
                    if (auto it = std::find_if(inputs.begin(), inputs.end(), [&](const auto& v) { return node.HashValue == v.Hash; }); it != inputs.end()) {
                        variableFrequencies[it->Index]++;
                        totalVars++;
                    } else {
                        fmt::print("Could not find variable {} with hash {} and calculated hash {} within the inputs\n", node.Name(), node.HashValue, node.CalculatedHashValue);
                        std::exit(EXIT_FAILURE);
                    }
                }
            }
        }
        for (const auto& v : inputs) {
            fmt::print("{}\t{:.3f}%\n", ds.GetName(v.Hash), static_cast<Operon::Scalar>(variableFrequencies[v.Index]) / totalVars);
        }
    }

    SECTION("Tree length histogram")
    {
        std::vector<size_t> lengthHistogram(maxLength + 1);
        for (auto& tree : trees) {
            lengthHistogram[tree.Length()]++;
        }
        fmt::print("Tree length histogram:\n");
        for (auto i = 1u; i < lengthHistogram.size(); ++i) {
            fmt::print("{}\t{}\n", i, lengthHistogram[i]);
        }
    }

    SECTION("Tree level size")
    {
        std::vector<double> levels;
        size_t minlevel = 0, maxlevel = 0;
        for (auto& tree : trees) {
            const auto& nodes = tree.Nodes();
            for (size_t i = 0; i < nodes.size(); ++i) {
                auto level = tree.Level(i);
                minlevel = std::min(minlevel, level);
                maxlevel = std::max(maxlevel, level);
                if (level < levels.size()) {
                    ++levels[level];
                } else {
                    levels.push_back(0);
                }
            }
        }
        fmt::print("min level: {}, max level: {}\n", minlevel, maxlevel);
        std::transform(levels.begin(), levels.end(), levels.begin(), [&](double d) { return d / trees.size(); });
        for (size_t i = 0; i < levels.size(); ++i) {
            fmt::print("{} {}\n", i + 1, levels[i]);
        }
    }

    SECTION("Tree depth histogram")
    {
        auto [minDep, maxDep] = std::minmax_element(trees.begin(), trees.end(), [](const auto& lhs, const auto& rhs) { return lhs.Depth() < rhs.Depth(); });
        std::vector<size_t> depthHistogram(maxDep->Depth() + 1);
        for (auto& tree : trees) {
            depthHistogram[tree.Depth()]++;
        }
        fmt::print("Tree depth histogram:\n");
        for (auto i = 0u; i < depthHistogram.size(); ++i) {
            auto v = depthHistogram[i];
            if (v == 0)
                continue;
            fmt::print("{}\t{}\n", i, depthHistogram[i]);
        }
    }
}

TEST_CASE("Tree initialization (uniform)", "[implementation]")
{
    auto target = "Y";
    auto ds = Dataset("../data/Poly-10.csv", true);
    auto variables = ds.Variables();
    std::vector<Variable> inputs;
    std::copy_if(variables.begin(), variables.end(), std::back_inserter(inputs), [&](auto& v) { return v.Name != target; });

    size_t maxDepth = 1000, maxLength = 100;

    const size_t nTrees = 100'000;

    auto sizeDistribution = std::uniform_int_distribution<size_t>(1, maxLength);
    //auto sizeDistribution = std::normal_distribution<Operon::Scalar> { maxLength / 2.0, 10 };
    auto creator = UniformTreeCreator(sizeDistribution, maxDepth, maxLength);
    Grammar grammar;
    grammar.SetConfig(Grammar::Arithmetic | NodeType::Log | NodeType::Exp);
    //Operon::Random rd(std::random_device {}());
    Operon::Random random(1234);

    auto trees = std::vector<Tree>(nTrees);
    std::generate(trees.begin(), trees.end(), [&]() { return creator(random, grammar, inputs); });

    auto totalLength = std::transform_reduce(std::execution::par_unseq, trees.begin(), trees.end(), 0.0, std::plus<size_t> {}, [](const auto& tree) { return tree.Length(); });
    auto totalShape = std::transform_reduce(std::execution::par_unseq, trees.begin(), trees.end(), 0.0, std::plus<size_t> {}, [](const auto& tree) { return tree.VisitationLength(); });
    fmt::print("Balanced tree creator - length({},{}) = {}\n", maxDepth, maxLength, totalLength / trees.size());
    fmt::print("Balanced tree creator - shape({},{}) = {}\n", maxDepth, maxLength, totalShape / trees.size());

    SECTION("Symbol frequencies")
    {
        std::array<size_t, NodeTypes::Count> symbolFrequencies;
        symbolFrequencies.fill(0u);
        for (const auto& tree : trees) {
            for (const auto& node : tree.Nodes()) {
                symbolFrequencies[NodeTypes::GetIndex(node.Type)]++;
            }
        }
        fmt::print("Symbol frequencies: \n");

        for (size_t i = 0; i < symbolFrequencies.size(); ++i) {
            auto node = Node(static_cast<NodeType>(1u << i));
            if (!grammar.IsEnabled(node.Type))
                continue;
            fmt::print("{}\t{:.3f} %\n", node.Name(), symbolFrequencies[i] / totalLength);
        }
    }

    SECTION("Symbol frequency vs tree length")
    {
        Eigen::MatrixXd counts = Eigen::MatrixXd::Zero(maxLength, NodeTypes::Count);
        std::cout << "\t";
        for (size_t i = 0; i < NodeTypes::Count; ++i) {
            std::cout << Node(static_cast<NodeType>(1u << i)).Name() << "\t";
        }
        std::cout << "\n";

        for (const auto& tree : trees) {
            for (const auto& node : tree.Nodes()) {
                Expects(grammar.IsEnabled(node.Type));
                counts(tree.Length() - 1, NodeTypes::GetIndex(node.Type))++;
            }
        }
        std::cout << counts << "\n";
    }

    SECTION("Variable frequencies")
    {
        fmt::print("Variable frequencies:\n");
        size_t totalVars = 0;
        std::vector<size_t> variableFrequencies(inputs.size());
        for (const auto& t : trees) {
            for (const auto& node : t.Nodes()) {
                if (node.IsVariable()) {
                    if (auto it = std::find_if(inputs.begin(), inputs.end(), [&](const auto& v) { return node.HashValue == v.Hash; }); it != inputs.end()) {
                        variableFrequencies[it->Index]++;
                        totalVars++;
                    } else {
                        fmt::print("Could not find variable {} with hash {} and calculated hash {} within the inputs\n", node.Name(), node.HashValue, node.CalculatedHashValue);
                        std::exit(EXIT_FAILURE);
                    }
                }
            }
        }
        for (const auto& v : inputs) {
            fmt::print("{}\t{:.3f}%\n", ds.GetName(v.Hash), static_cast<Operon::Scalar>(variableFrequencies[v.Index]) / totalVars);
        }
    }

    SECTION("Tree length histogram")
    {
        std::vector<size_t> lengthHistogram(maxLength + 1);
        for (auto& tree : trees) {
            lengthHistogram[tree.Length()]++;
        }
        fmt::print("Tree length histogram:\n");
        for (auto i = 1u; i < lengthHistogram.size(); ++i) {
            fmt::print("{}\t{}\n", i, lengthHistogram[i]);
        }
    }

    SECTION("Tree depth histogram")
    {
        auto [minDep, maxDep] = std::minmax_element(trees.begin(), trees.end(), [](const auto& lhs, const auto& rhs) { return lhs.Depth() < rhs.Depth(); });
        std::vector<size_t> depthHistogram(maxDep->Depth() + 1);
        for (auto& tree : trees) {
            depthHistogram[tree.Depth()]++;
        }
        fmt::print("Tree depth histogram:\n");
        for (auto i = 0u; i < depthHistogram.size(); ++i) {
            auto v = depthHistogram[i];
            if (v == 0)
                continue;
            fmt::print("{}\t{}\n", i, depthHistogram[i]);
        }
    }

    SECTION("Shape balancing")
    {
        auto crossover = SubtreeCrossover(0.5, maxDepth, maxLength);
        auto oldShape = totalShape / trees.size();
        int steps = 0;

        while (steps < 5) {
            std::shuffle(trees.begin(), trees.end(), random);
            for (size_t i = 0; i < trees.size() - 1; i += 2) {
                auto j = i + 1;
                auto [x, y] = crossover.FindCompatibleSwapLocations(random, trees[i], trees[j]);
                auto c1 = crossover.Cross(trees[i], trees[j], x, y);
                auto c2 = crossover.Cross(trees[j], trees[i], y, x);
                std::swap(trees[i], c1);
                std::swap(trees[j], c2);
            }
            auto newShape = std::transform_reduce(std::execution::par_unseq, trees.begin(), trees.end(), 0.0, std::plus<size_t> {}, [](const auto& tree) { return tree.VisitationLength(); }) / trees.size();
            if (newShape < oldShape)
                oldShape = newShape;
            else
                ++steps;
            fmt::print("new shape: {}\n", newShape);
        }
    }
}

TEST_CASE("Tree depth calculation", "[implementation]")
{
    auto target = "Y";
    auto ds = Dataset("../data/Poly-10.csv", true);
    auto variables = ds.Variables();
    std::vector<Variable> inputs;
    std::copy_if(variables.begin(), variables.end(), std::back_inserter(inputs), [&](auto& v) { return v.Name != target; });
    size_t maxDepth = 20, maxLength = 50;

    auto sizeDistribution = std::uniform_int_distribution<size_t>(2, maxLength);
    auto creator = BalancedTreeCreator(sizeDistribution, maxDepth, maxLength);
    Grammar grammar;
    Operon::Random rd(std::random_device {}());

    //fmt::print("Min function arity: {}\n", grammar.MinimumFunctionArity());

    auto tree = creator(rd, grammar, inputs);
    fmt::print("{}\n", TreeFormatter::Format(tree, ds));
}
}
