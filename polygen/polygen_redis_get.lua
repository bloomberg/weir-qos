-- Copyright 2024 Bloomberg Finance L.P.
-- Distributed under the terms of the Apache 2.0 license.
local result = {}
local num_keys = table.getn(KEYS)
for i = 1,num_keys,1 do
    if string.sub(KEYS[i], 1, 4) == "verb" then
        local fields = redis.call('hgetall', KEYS[i])
        table.insert(result, fields)
    elseif string.sub(KEYS[i], 1, 4) == "conn" then
        -- We're in the process  of migrating the API here,
        -- in the old API connections were stored as a set of keys (one per connection)
        -- in the new API connections are stored as a simple count.
        -- Once the old API usages have been cleaned up we can remove this check
        local keytype = redis.call('type', KEYS[i])['ok']
        if  keytype == 'set' then
            -- remove expired connections from the set
            local items = redis.call('smembers', KEYS[i])
            local len = table.getn(items)
            for j = 1,len,1 do
                local exist = redis.call('exists', items[j])
                if exist == 0 then
                    redis.call('srem', KEYS[i], items[j])
                end
            end

            local fields = redis.call('scard', KEYS[i])
            table.insert(result, fields)
        elseif keytype == 'string' then
            local value = redis.call('get', KEYS[i])
            table.insert(result, value)
        elseif keytype == 'none' then
            -- The key may have been removed since we got the list of keys
            table.insert(result, 0)
        else
            table.insert(result, 'ERROR: Unrecognised active-request key type '..keytype)
        end
    end
end
return result
