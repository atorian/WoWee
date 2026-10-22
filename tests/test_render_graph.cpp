#include <catch_amalgamated.hpp>
#include "rendering/render_graph.hpp"
#include <algorithm>

using namespace wowee::rendering;

TEST_CASE("A cyclic frame cannot execute a guessed order", "[render-graph]") {
    RenderGraph graph;
    const auto geometry=graph.registerResource("geometry");
    const auto light=graph.registerResource("light");
    graph.addPass("geometry", {light}, {geometry}, [](auto) {});
    graph.addPass("lighting", {geometry}, {light}, [](auto) {});
    graph.compile();
    CHECK(graph.getExecutionOrder().empty());
}

TEST_CASE("Ambiguous resource producers cannot silently replace one another", "[render-graph]") {
    RenderGraph graph;
    const auto geometry=graph.registerResource("geometry");
    graph.addPass("geometry", {}, {geometry}, [](auto) {});
    graph.addPass("lighting", {geometry}, {}, [](auto) {});
    graph.addPass("overwrite", {}, {geometry}, [](auto) {});
    graph.compile();
    CHECK(graph.getExecutionOrder().empty());
}

TEST_CASE("Frame dependencies reorder callbacks independently of registration", "[render-graph]") {
    RenderGraph graph;
    const auto geometry=graph.registerResource("geometry");
    const auto trace=graph.registerResource("trace");
    const auto light=graph.registerResource("light");
    graph.addPass("lighting", {light}, {}, [](auto) {});
    graph.addPass("resolve", {trace,geometry}, {light}, [](auto) {});
    graph.addPass("trace", {geometry}, {trace}, [](auto) {});
    graph.addPass("geometry", {}, {geometry}, [](auto) {});
    graph.compile();
    std::vector<std::string> names;
    for(auto id:graph.getExecutionOrder()) names.push_back(graph.getPasses()[id].name);
    CHECK(names==std::vector<std::string>{"geometry","trace","resolve","lighting"});
}

TEST_CASE("The next writer waits for every reader of the previous version", "[render-graph]") {
    RenderGraph graph;
    const auto first=graph.registerResource("color");
    const auto second=graph.versionResource(first,"color with water");
    std::vector<std::string> executed;
    graph.addPass("water", {first}, {second}, [&](auto) { executed.push_back("water"); });
    graph.addPass("capture", {first}, {}, [&](auto) { executed.push_back("capture"); });
    graph.addPass("opaque", {}, {first}, [&](auto) { executed.push_back("opaque"); });
    graph.addPass("present", {second}, {}, [&](auto) { executed.push_back("present"); });
    REQUIRE(graph.compile());
    graph.record(VK_NULL_HANDLE); // logical callbacks need no device
    CHECK(executed==std::vector<std::string>{"opaque","capture","water","present"});
}

TEST_CASE("Image uses synchronize geometry, compute tracing and fragment lighting", "[render-graph]") {
    RenderGraph graph;
    auto geometry=graph.registerResource("geometry");
    auto light=graph.registerResource("light");
    const VkImage image=(VkImage)(uintptr_t)1, lightImage=(VkImage)(uintptr_t)2;
    graph.importImage(geometry,image,{});
    graph.importImage(light,lightImage,{});
    auto lighting=graph.addPass("lighting", {light,geometry}, {}, [](auto) {});
    auto trace=graph.addPass("trace", {geometry}, {light}, [](auto) {});
    auto draw=graph.addPass("geometry", {}, {geometry}, [](auto) {});
    const RGState sample{VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    graph.use(draw,geometry,{VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    graph.use(trace,geometry,{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    graph.use(trace,light,{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,VK_IMAGE_LAYOUT_GENERAL});
    graph.use(lighting,geometry,sample); graph.use(lighting,light,sample);
    REQUIRE(graph.compile());
    const auto& beforeTrace=graph.getPasses()[trace].imageBarriers;
    REQUIRE(beforeTrace.size()==2);
    CHECK(beforeTrace[0].image==image);
    CHECK(beforeTrace[0].srcAccess==VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    CHECK(beforeTrace[0].dstStage==VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    CHECK(beforeTrace[0].oldLayout==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    const auto& beforeLight=graph.getPasses()[lighting].imageBarriers;
    REQUIRE(beforeLight.size()==2); // compute visibility doesn't cover fragment reads
    CHECK(beforeLight[0].srcStage==VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    CHECK(beforeLight[0].dstStage==VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    CHECK(beforeLight[1].srcAccess==VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    CHECK(beforeLight[1].newLayout==VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // Recompilation cannot append a second set of transitions.
    REQUIRE(graph.compile());
    CHECK(graph.getPasses()[lighting].imageBarriers.size()==2);
}

TEST_CASE("A disabled producer cannot expose undefined GPU contents", "[render-graph]") {
    RenderGraph graph;
    auto light=graph.registerResource("light");
    graph.importImage(light,(VkImage)(uintptr_t)1,{});
    auto write=graph.addPass("trace", {}, {light}, [](auto) {});
    auto read=graph.addPass("lighting", {light}, {}, [](auto) {});
    graph.use(write,light,{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,VK_ACCESS_2_SHADER_WRITE_BIT,VK_IMAGE_LAYOUT_GENERAL});
    graph.use(read,light,{VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT,VK_IMAGE_LAYOUT_GENERAL});
    graph.setPassEnabled("trace",false);
    CHECK_FALSE(graph.compile());
    CHECK(graph.getExecutionOrder().empty());
    graph.setPassEnabled("trace",true);
    CHECK(graph.compile());
    graph.reset();
    auto stale=graph.addPass("lighting", {light}, {}, [](auto) {});
    graph.use(stale,light,{VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_READ_BIT,VK_IMAGE_LAYOUT_GENERAL});
    CHECK_FALSE(graph.compile()); // next slot must supply its own image
}

TEST_CASE("Mutation invalidates a compiled logical frame", "[render-graph]") {
    RenderGraph graph;
    int count=0;
    graph.addPass("first", {}, {}, [&](auto) { ++count; });
    REQUIRE(graph.compile());
    graph.addPass("second", {}, {}, [&](auto) { count+=10; });
    graph.record(VK_NULL_HANDLE);
    CHECK(count==11);
    graph.setPassEnabled("first",false);
    graph.record(VK_NULL_HANDLE);
    CHECK(count==21);
}
