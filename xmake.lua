set_project("libmcp")
set_version("0.0.1")
set_xmakever("2.8.3")

add_rules("mode.debug", "mode.release")
set_languages("cxx17")

option("with_tests")
    set_default(false)
    set_showmenu(true)
    set_description("Enable libmcp_unittest target (pulls gtest)")
option_end()

-- libca 依赖经公开包定义仓拉取（首次配置自动克隆）。
add_repositories("luiox-repo https://github.com/luiox/luiox-repo.git")
add_requires("libca 0.0.7")

target("libmcp")
    set_kind("static")
    add_packages("libca", {public = true})
    add_files("src/mcp/*.cpp")
    add_headerfiles("src/mcp/**.hpp")
    add_includedirs("src", {public = true})

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
target_end()

if has_config("with_tests") then
    add_requires("gtest")

    target("libmcp_unittest")
        set_kind("binary")
        set_default(false)

        add_deps("libmcp")
        add_packages("gtest", "libca")
        add_files("unittest/main.cpp")
        add_files("unittest/*_test.cpp")
        add_includedirs("src")
        set_rundir("$(projectdir)")

        if is_plat("windows") then
            add_cxflags("/utf-8", {tools = "cl"})
        end
        if is_plat("linux") then
            add_syslinks("pthread")
        end
target_end()
end
