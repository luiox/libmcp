target("libmcp")
    set_kind("static")
    set_group("libs")

    add_deps("libca_core", "libca_str", "libca_json", "libca_io")
    add_files("src/mcp/*.cpp")
    add_headerfiles("src/mcp/**.hpp")
    add_includedirs("src", {public = true})

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

if has_config("with_tests") then
    target("libmcp_unittest")
        set_kind("binary")
        set_group("libs/test")
        set_default(false)

        add_deps("libmcp", "test_helper")
        add_links("libmcp", "libca_json", "libca_str", "libca_io", "libca_core")
        add_packages("gtest")
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
end
