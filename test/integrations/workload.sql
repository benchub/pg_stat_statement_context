-- One iteration of the sample workload of scripts/test-integrations.sh, run
-- in a loop as role "app" in database "shop". The checks rely on the ratios:
-- users#show : posts#index = 5 : 2, and 3 nested statements per orders#create.
SELECT name FROM users WHERE id = 1 /*controller='users',action='show'*/;
SELECT name FROM users WHERE id = 2 /*controller='users',action='show'*/;
SELECT name FROM users WHERE id = 3 /*controller:users,action:show*/;
SELECT name FROM users WHERE id = 4 /*controller:users,action:show*/;
SELECT name FROM users WHERE id = 5 /*controller:users,action:show*/;
SELECT id, title FROM posts WHERE user_id = 7 ORDER BY id LIMIT 20 /*controller='posts',action='index'*/;
SELECT id, title FROM posts WHERE user_id = 8 ORDER BY id LIMIT 20 /*controller='posts',action='index'*/;
SELECT create_order(9) /*controller='orders',action='create'*/;
DELETE FROM orders WHERE created < now() - interval '1 minute' /*job='cleanup'*/;
-- An invalid tag (0xFF is not UTF-8): counted in _info().invalid_tags; the
-- statement is still recorded with the remaining tag, action=check.
SELECT 1 /*controller='%FF',action='check'*/;
