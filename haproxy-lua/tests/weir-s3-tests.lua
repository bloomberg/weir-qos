-- this is to mock off core class from haproxy lua
core = {register_fetches = function () return nil end,
        register_service = function() return nil end,
        Debug = function(msg) print("DEBUG: "..msg) return nil end,
        Info = function(msg) print("INFO: "..msg) return nil end,
        Warning = function(msg) print("WARN: "..msg) return nil end,
    }

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

test_get_access_key_from_header = {}
    function test_get_access_key_from_header:tests()
        local query_params = {}
        local headers = {["authorization"] = {[0] = "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request"}}
        lu.assertEquals(get_access_key(headers, query_params), "AKIAIOSFODNN7EXAMPLE")
        headers = {["authorization"] = {[0] = "AWS AKIAIOSFODNN7EXAMPLE:frJIUN8DYpKDtOLCwo//yllqDzg="}}
        lu.assertEquals(get_access_key(headers, query_params), "AKIAIOSFODNN7EXAMPLE")
        headers = {["authorization"] = {[0] = "AWS5 AKIAIOSFODNN7EXAMPLE:frJIUN8DYpKDtOLCwo//yllqDzg="}}
        lu.assertEquals(get_access_key(headers, query_params), "InvalidAuthorization")
        headers = {["authorization"] = {[0] = "AWS NotTwentyChars:frJIUN8DYpKDtOLCwo//yllqDzg="}}
        lu.assertEquals(get_access_key(headers, query_params), "ItIsInvalidAccessKey")
        headers = {["authorization"] = {[0] = "AWS With_SpecialChar:frJIUN8DYpKDtOLCwo//yllqDzg="}}
        lu.assertEquals(get_access_key(headers, query_params), "ItIsInvalidAccessKey")
    end

test_get_access_key_from_query_string = {}
    function test_get_access_key_from_query_string:tests()
        local headers = {}
        local query_params = {}
        lu.assertEquals(get_access_key(headers, query_params), "common")

        -- v4
        lu.assertEquals(get_access_key(headers, get_decoded_query_params("X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=AKIAIOSFODNN7EXAMPLE/20221205/us-east-1/s3/aws4_request&X-Amz-Date=20221205T184410Z&X-Amz-Expires=3600&X-Amz-SignedHeaders=host&X-Amz-Signature=signature")), "AKIAIOSFODNN7EXAMPLE")
        lu.assertEquals(get_access_key(headers, get_decoded_query_params("X-AMZ-CREDENTIAL=AKIAIOSFODNN7EXAMPLE/20221205/us-east-1/s3/aws4_request")), "AKIAIOSFODNN7EXAMPLE")
        lu.assertEquals(get_access_key(headers, get_decoded_query_params("X-AMZ-CREDENTIAL=AKIAIOSFODNN7EXAMPLE%2F20221205%2Fus-east-1%2Fs3%2Faws4_request")), "AKIAIOSFODNN7EXAMPLE")
        lu.assertEquals(get_access_key(headers, get_decoded_query_params("X-amz-CRedentiAL=AKIAIOSFODNN7EXAMPLE")), "AKIAIOSFODNN7EXAMPLE")
        lu.assertEquals(get_access_key(headers, get_decoded_query_params("")), "common")
        lu.assertEquals(get_access_key(headers, get_decoded_query_params(" ")), "common")
        lu.assertEquals(get_access_key(headers, get_decoded_query_params("a=1&b=2")), "common")
        lu.assertEquals(get_access_key(headers, get_decoded_query_params("X-Amz-Credential=TwentyWith/SpecialCh")), "ItIsInvalidAccessKey")
        lu.assertEquals(get_access_key(headers, get_decoded_query_params("X-Amz-Credential=AlphaNumButMoreThanTwentyChars")), "ItIsInvalidAccessKey")

        -- v2
        lu.assertEquals(get_access_key(headers, get_decoded_query_params("AWSAccessKeyId=AKIAIOSFODNN7EXAMPLE&Action=DescribeJobFlows&SignatureMethod=HmacSHA256&SignatureVersion=2&Timestamp=2011-10-03T15%3A19%3A30&Version=2009-03-31&Signature=calculated value")), "AKIAIOSFODNN7EXAMPLE")
        lu.assertEquals(get_access_key(headers, get_decoded_query_params("AWSACCESSKEYID=AKIAIOSFODNN7EXAMPLE&")), "AKIAIOSFODNN7EXAMPLE")
    end


os.exit(lu.LuaUnit.run())
