-- Copyright 2024 Bloomberg Finance L.P.
-- Distributed under the terms of the Apache 2.0 license.

local limit_share_updates = {}
-- this is to mock off core class from haproxy lua
core = {register_fetches = function () return nil end,
        register_service = function() return nil end,
        register_action = function() return nil end,
        Debug = function(msg) print("DEBUG: "..msg) return nil end,
        Info = function(msg) print("INFO: "..msg) return nil end,
        Warning = function(msg) print("WARN: "..msg) return nil end,

        ingest_weir_limit_share_update = function(timestamp, user_key, instance_id, direction, limit)
            table.insert(limit_share_updates, {timestamp, user_key, instance_id, direction, limit})
        end,
    }

require("haproxy_lua")
local lu = require("luaunit")

mock_applet = {
    lines = {}, -- Set this before each test
    getline = function(self)
        local result = self.lines[1]
        if #self.lines == 0 then
            return nil
        end
        table.remove(self.lines, 1)
        return result
    end,
    send = function(self, msg) end
}

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

test_get_bucket_name = {}
    function test_get_bucket_name:tests()
        lu.assertEquals(get_bucket_name("/", "bucket1.s3.dev.com"), "bucket1")
        lu.assertEquals(get_bucket_name("/", "bucket1.S3.dev.com"), "bucket1")
        lu.assertEquals(get_bucket_name("/bucket1", "www.google.com"), "bucket1")
        lu.assertEquals(get_bucket_name("/bucket1/obj1", "s3.dev.com"), "bucket1")
        lu.assertEquals(get_bucket_name("/bucket1", "s3.dev.com"), "bucket1")
    end

test_is_reqs_violater = {}
    reqs_map['access_key1'] = 123456
    function test_is_reqs_violater:tests()
        does_violate, vio_type = is_violater(123456, "access_key1", nil)
        lu.assertEquals(does_violate, 1)
        lu.assertEquals(vio_type, "requests")
    end

test_update_violates = {}
    function test_update_violates:test_violates_map()
        update_violates("1554318336056379,user_GET,access_key2", 1554318336)
        lu.assertEquals(vio_map["user_GET"][1554318336]["access_key2"], 1)
    end
    function test_update_violates:test_violates_reqs()
        update_violates("user_reqs_block,access_key3",1554318336)
        lu.assertEquals(reqs_map["access_key3"], 1554318336)

        does_violate, vio_type = is_violater(1554318336, "access_key3", "GET")
        lu.assertEquals(does_violate, 1)
        lu.assertEquals(vio_type, "requests")

        update_violates("user_reqs_unblock,access_key3",1554318336)
        does_violate, vio_type = is_violater(1554318336, "access_key3", "GET")
        lu.assertEquals(does_violate, 0)
        lu.assertEquals(vio_type, "")
    end

test_ingest_policies_successfully_parses_limit_share_updates = function()
    mock_applet.lines = {
        "limit_share",
        "12345,key1,fff1_dwn_64,fff1_up_16,fff2_dwn_10240,fff2_up_10241",
        "12346,key2,fff1_dwn_64",
        "end_limit_share",
    }
    limit_share_updates = {}

    ingest_policies(mock_applet)

    lu.assertEquals(limit_share_updates, {
        {12345, "key1", "fff1", "dwn", 64},
        {12345, "key1", "fff1", "up", 16},
        {12345, "key1", "fff2", "dwn", 10240},
        {12345, "key1", "fff2", "up", 10241},
        {12346, "key2", "fff1", "dwn", 64},
    })
end

test_ingest_policies_ignores_remaining_limit_share_updates_when_one_is_too_short = function()
    mock_applet.lines = {
        "limit_share",
        "12345,key1,fff1_dwn_64,fff2_down,fff2_up_10241", -- Second component is missing a quantity
        "12346,key2,fff3_dwn_65",
        "end_limit_share",
    }
    limit_share_updates = {}

    ingest_policies(mock_applet)

    lu.assertEquals(limit_share_updates, {
        {12345, "key1", "fff1", "dwn", 64},
    })
end

test_ingest_policies_ignores_remaining_limit_share_updates_when_one_has_invalid_timestamp = function()
    mock_applet.lines = {
        "limit_share",
        "1234F,key1,fff1_dwn_64,fff2_up_10241", -- Timestamp is not a valid base-10 integer
        "12346,key2,fff3_dwn_65",
        "end_limit_share",
    }
    limit_share_updates = {}

    ingest_policies(mock_applet)

    lu.assertEquals(limit_share_updates, {})
end

test_ingest_policies_ignores_remaining_limit_share_updates_when_one_has_invalid_limit_quantity = function()
    mock_applet.lines = {
        "limit_share",
        "12345,key1,fff1_dwn_64,fff2_down_11b,fff2_up_10241", -- Second component's quantity is not a valid base-10 integer
        "12346,key2,fff3_dwn_65",
        "end_limit_share",
    }
    limit_share_updates = {}

    ingest_policies(mock_applet)

    lu.assertEquals(limit_share_updates, {
        {12345, "key1", "fff1", "dwn", 64},
    })
end

test_ingest_policies_gracefully_recovers_from_invalid_limit_share_message = function()
    mock_applet.lines = {
        "limit_share",
        "12345,key1,fff1_dwn_64",
        "random-garbage-that-isnt-a-valid-update",
        "12345,key1,fff2_dwn_16",
        "limit_share",
        "12346,key2,fff3_dwn_65",
        "end_limit_share",
    }
    limit_share_updates = {}

    ingest_policies(mock_applet)

    lu.assertEquals(limit_share_updates, {
        {12345, "key1", "fff1", "dwn", 64},
        {12346, "key2", "fff3", "dwn", 65},
    })
end

test_ingest_policies_gracefully_recovers_from_unexpected_end_of_limit_share_message = function()
    mock_applet.lines = {
        "limit_share",
        "12345,key1,fff1_dwn_64",
        "limit_share",
        "12346,key2,fff3_dwn_65",
        "end_limit_share",
    }
    limit_share_updates = {}

    ingest_policies(mock_applet)

    lu.assertEquals(limit_share_updates, {
        {12345, "key1", "fff1", "dwn", 64},
        {12346, "key2", "fff3", "dwn", 65},
    })
end
os.exit(lu.LuaUnit.run())
