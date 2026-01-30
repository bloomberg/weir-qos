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

test_string_endswith = {}
    function test_string_endswith:tests()
        lu.assertEquals(string_endswith("", ""), true)
        lu.assertEquals(string_endswith("", "a"), false)
        lu.assertEquals(string_endswith("a", ""), true)
        lu.assertEquals(string_endswith("a", "a"), true)
        lu.assertEquals(string_endswith("a", "A"), false) -- case sensitive
        lu.assertEquals(string_endswith("a", "aa"), false)
        lu.assertEquals(string_endswith("abc", ""), true)
        lu.assertEquals(string_endswith("abc", "ab"), false) -- off by one
        lu.assertEquals(string_endswith("abc", "c"), true)
        lu.assertEquals(string_endswith("abc", "bc"), true)
        lu.assertEquals(string_endswith("abc", "abc"), true)
        lu.assertEquals(string_endswith("abc", " abc"), false)
        lu.assertEquals(string_endswith("abc", "a"), false)
        lu.assertEquals(string_endswith("abc", "."), false) -- Lua pattern char
        lu.assertEquals(string_endswith("ab\0c", "b\0c"), true)     -- \0
        lu.assertEquals(string_endswith("ab\0c", "b\0d"), false) -- \0
        lu.assertError(string_endswith, "abc", nil)
        lu.assertError(string_endswith, nil, "c")
    end

test_string_split = {}
    function test_string_split:tests()
        lu.assertEquals(string_split('', ''), {''})
        lu.assertEquals(string_split('', 'z'), {})
        lu.assertEquals(string_split('a', ''), {'a'})
        lu.assertEquals(string_split('a', 'a'), {'',''})
        lu.assertEquals(string_split('abc', 'abc'), {'',''})
        lu.assertEquals(string_split('xabcy', 'abc'), {'x', 'y'})
        lu.assertEquals(string_split(' 1  2  3 ',' '),{'','1','','2','','3',''})
        lu.assertEquals(string_split('a*bb*c*ddd','*'),{'a','bb','c','ddd'})
        lu.assertEquals(string_split('dog:fred:bonzo:alice',':',3), {'dog','fred','bonzo:alice'})
        lu.assertEquals(string_split('a///','/'),{'a','','',''})
        lu.assertEquals(string_split('/a//','/'),{'','a','',''})
        lu.assertEquals(string_split('//a/','/'),{'','','a',''})
        lu.assertEquals(string_split('///a','/'),{'','','','a'})
        lu.assertEquals(string_split('///','/'),{'','','',''})
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
