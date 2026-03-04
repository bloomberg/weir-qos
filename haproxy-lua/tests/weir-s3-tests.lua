-- this is to mock off core class from haproxy lua
core = {register_fetches = function () return nil end,
        register_service = function() return nil end,
        register_action = function() return nil end,
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

-- STS QoS tests starts here 
-- Tests for is_sts_credential_req

TestIfStsCredentialReq = {}

function TestIfStsCredentialReq:test_returns_1_when_security_token_header_present()
    local headers = {
        ["x-amz-security-token"] = { [0] = "some-sts-token-value" }
    }
    lu.assertEquals(is_sts_credential_req(headers), 1)
end

function TestIfStsCredentialReq:test_returns_0_when_no_security_token_header()
    local headers = {
        ["authorization"] = { [0] = "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request" }
    }
    lu.assertEquals(is_sts_credential_req(headers), 0)
end

function TestIfStsCredentialReq:test_returns_0_when_empty_headers()
    local headers = {}
    lu.assertEquals(is_sts_credential_req(headers), 0)
end

-- Tests for sts_qos_populate_txn_context

TestStsQosPopulateTxnContext = {}

-- Create a mock table to run unit tests for sts_qos_populate_txn_context since it relies on the txn object provided by HAProxy, which is not available in a standalone Lua environment. 
-- This mock will simulate the necessary methods and properties of the txn object for testing purposes.
local function make_mock_txn(opts)
    local vars = {}
    return {
        http = {
            req_get_headers = function()
                return opts.headers or {}
            end,
        },
        sf = {
            req_fhdr = function(self, name)
                if name == "Content-Length" then
                    return opts.content_length
                end
                return nil
            end,
        },
        f = {
            path = function() return opts.path or "/" end,
            req_body_param = function() return opts.req_body_param end,
        },
        set_var = function(self, name, value)
            vars[name] = value
        end,
        get_var = function(self, name)
            return vars[name]
        end,
        _vars = vars,
    }
end

function TestStsQosPopulateTxnContext:test_sets_if_body_parse_for_assume_role()
    local txn = make_mock_txn({
        content_length = "128",
        path = "/",
        req_body_param = "AssumeRole",
        headers = {},
    })

    sts_qos_populate_txn_context(txn)

    lu.assertEquals(txn:get_var("txn.if_body_parse"), "yes")
end

function TestStsQosPopulateTxnContext:test_does_not_set_if_body_parse_for_non_assume_role()
    local txn = make_mock_txn({
        content_length = "128",
        path = "/",
        req_body_param = "NotAssumeRole",
        headers = {},
    })

    sts_qos_populate_txn_context(txn)

    lu.assertNil(txn:get_var("txn.if_body_parse"))
end

function TestStsQosPopulateTxnContext:test_does_not_set_if_body_parse_when_no_content_length()
    local txn = make_mock_txn({
        content_length = nil,
        path = "/",
        req_body_param = "AssumeRole",
        headers = {},
    })

    sts_qos_populate_txn_context(txn)

    lu.assertNil(txn:get_var("txn.if_body_parse"))
end

function TestStsQosPopulateTxnContext:test_does_not_set_if_body_parse_when_content_length_zero()
    local txn = make_mock_txn({
        content_length = "0",
        path = "/",
        req_body_param = "AssumeRole",
        headers = {},
    })

    sts_qos_populate_txn_context(txn)

    lu.assertNil(txn:get_var("txn.if_body_parse"))
end

function TestStsQosPopulateTxnContext:test_does_not_set_if_body_parse_when_path_is_not_root()
    local txn = make_mock_txn({
        content_length = "128",
        path = "/some/other/path",
        req_body_param = "AssumeRole",
        headers = {},
    })

    sts_qos_populate_txn_context(txn)

    lu.assertNil(txn:get_var("txn.if_body_parse"))
end

function TestStsQosPopulateTxnContext:test_does_not_set_if_body_parse_when_body_param_nil()
    local txn = make_mock_txn({
        content_length = "128",
        path = "/",
        req_body_param = nil,
        headers = {},
    })

    sts_qos_populate_txn_context(txn)

    lu.assertNil(txn:get_var("txn.if_body_parse"))
end

function TestStsQosPopulateTxnContext:test_logs_sts_token_when_present()
    local logged_messages = {}
    core.Info = function(msg) table.insert(logged_messages, msg) end

    local txn = make_mock_txn({
        content_length = nil,
        path = "/",
        req_body_param = nil,
        headers = {
            ["x-amz-security-token"] = { [0] = "some-token" },
            ["authorization"] = { [0] = "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request" },
        },
    })

    sts_qos_populate_txn_context(txn)

    lu.assertEquals(#logged_messages, 1)
    lu.assertStrContains(logged_messages[1], "accesskeyid-ststoken")

    -- Restore original core.Info
    core.Info = function() return nil end
end
-- STS QoS tests ends here 

os.exit(lu.LuaUnit.run())
