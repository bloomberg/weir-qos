require("weir-s3")
local lu = require("luaunit")

test_get_bucket_name = {}
    function test_get_bucket_name:tests()
        lu.assertEquals(get_bucket_name("/", "bucket1.s3.dev.com"), "bucket1")
        lu.assertEquals(get_bucket_name("/", "bucket1.S3.dev.com"), "bucket1")
        lu.assertEquals(get_bucket_name("/bucket1", "www.google.com"), "bucket1")
        lu.assertEquals(get_bucket_name("/bucket1/obj1", "s3.dev.com"), "bucket1")
        lu.assertEquals(get_bucket_name("/bucket1", "s3.dev.com"), "bucket1")
    end

os.exit(lu.LuaUnit.run())
