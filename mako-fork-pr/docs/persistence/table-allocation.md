## implementation of table-allocations
We preallocate empty tables on shard servers (`mako::NUM_TABLES_PER_SHARD`); and assign the next available table to users when it's necessary. The table id starts from 1, and global unique. The table ids ready for each shard as follows:

```
shard-0: [1, mako::NUM_TABLES_PER_SHARD]
shard-1: [mako::NUM_TABLES_PER_SHARD+1, 2*mako::NUM_TABLES_PER_SHARD], 
shard-2: [2*mako::NUM_TABLES_PER_SHARD+1, 3*mako::NUM_TABLES_PER_SHARD], 
....
```

Replicated tables on followers and learners have the same table-id as the leader.


## previous implementation of table-allocations

### setup
2 replicated shards, with 6 worker threads on each shard server.

### leaders
`shard0-localhost.log / shard1-localhost.log`

`partitions`[table_type] = vector of `# of threads`
```
new table is created2: customer_0, id: 1
new table is created2: customer_1, id: 2
new table is created2: customer_2, id: 3
new table is created2: customer_3, id: 4
new table is created2: customer_4, id: 5
new table is created2: customer_5, id: 6
new table is created2: customer_name_idx_0, id: 7
new table is created2: customer_name_idx_1, id: 8
new table is created2: customer_name_idx_2, id: 9
new table is created2: customer_name_idx_3, id: 10
new table is created2: customer_name_idx_4, id: 11
new table is created2: customer_name_idx_5, id: 12
new table is created2: district_0, id: 13
new table is created2: district_1, id: 14
new table is created2: district_2, id: 15
new table is created2: district_3, id: 16
new table is created2: district_4, id: 17
new table is created2: district_5, id: 18
new table is created2: history_0, id: 19
new table is created2: history_1, id: 20
new table is created2: history_2, id: 21
new table is created2: history_3, id: 22
new table is created2: history_4, id: 23
new table is created2: history_5, id: 24
new table is created2: new_order_0, id: 25
new table is created2: new_order_1, id: 26
new table is created2: new_order_2, id: 27
new table is created2: new_order_3, id: 28
new table is created2: new_order_4, id: 29
new table is created2: new_order_5, id: 30
new table is created2: oorder_0, id: 31
new table is created2: oorder_1, id: 32
new table is created2: oorder_2, id: 33
new table is created2: oorder_3, id: 34
new table is created2: oorder_4, id: 35
new table is created2: oorder_5, id: 36
new table is created2: oorder_c_id_idx_0, id: 37
new table is created2: oorder_c_id_idx_1, id: 38
new table is created2: oorder_c_id_idx_2, id: 39
new table is created2: oorder_c_id_idx_3, id: 40
new table is created2: oorder_c_id_idx_4, id: 41
new table is created2: oorder_c_id_idx_5, id: 42
new table is created2: order_line_0, id: 43
new table is created2: order_line_1, id: 44
new table is created2: order_line_2, id: 45
new table is created2: order_line_3, id: 46
new table is created2: order_line_4, id: 47
new table is created2: order_line_5, id: 48
new table is created2: stock_0, id: 49
new table is created2: stock_1, id: 50
new table is created2: stock_2, id: 51
new table is created2: stock_3, id: 52
new table is created2: stock_4, id: 53
new table is created2: stock_5, id: 54
new table is created2: stock_data_0, id: 55
new table is created2: stock_data_1, id: 56
new table is created2: stock_data_2, id: 57
new table is created2: stock_data_3, id: 58
new table is created2: stock_data_4, id: 59
new table is created2: stock_data_5, id: 60
new table is created2: warehouse_0, id: 61
new table is created2: warehouse_1, id: 62
new table is created2: warehouse_2, id: 63
new table is created2: warehouse_3, id: 64
new table is created2: warehouse_4, id: 65
new table is created2: warehouse_5, id: 66
new table is created2: item, id: 67
```

dummy tables: OpenTablesForTablespaceDummy.
In total = partitions * nshards; item does not matter, as we don't really store data in dummy tables, we just use it for write/read set, and item ready-only.

`dummy_partitions`[table_type] = vector of `# of threads  *  # of shards`
```
new table is created2: customer_dummy_1, id: 68
new table is created2: customer_dummy_2, id: 69
new table is created2: customer_dummy_3, id: 70
new table is created2: customer_dummy_4, id: 71
new table is created2: customer_dummy_5, id: 72
new table is created2: customer_dummy_6, id: 73
new table is created2: customer_dummy_7, id: 74
new table is created2: customer_dummy_8, id: 75
new table is created2: customer_dummy_9, id: 76
new table is created2: customer_dummy_10, id: 77
new table is created2: customer_dummy_11, id: 78
new table is created2: customer_dummy_12, id: 79
new table is created2: customer_name_idx_dummy_1, id: 80
new table is created2: customer_name_idx_dummy_2, id: 81
new table is created2: customer_name_idx_dummy_3, id: 82
new table is created2: customer_name_idx_dummy_4, id: 83
new table is created2: customer_name_idx_dummy_5, id: 84
new table is created2: customer_name_idx_dummy_6, id: 85
new table is created2: customer_name_idx_dummy_7, id: 86
new table is created2: customer_name_idx_dummy_8, id: 87
new table is created2: customer_name_idx_dummy_9, id: 88
new table is created2: customer_name_idx_dummy_10, id: 89
new table is created2: customer_name_idx_dummy_11, id: 90
new table is created2: customer_name_idx_dummy_12, id: 91
new table is created2: district_dummy_1, id: 92
new table is created2: district_dummy_2, id: 93
new table is created2: district_dummy_3, id: 94
new table is created2: district_dummy_4, id: 95
new table is created2: district_dummy_5, id: 96
new table is created2: district_dummy_6, id: 97
new table is created2: district_dummy_7, id: 98
new table is created2: district_dummy_8, id: 99
new table is created2: district_dummy_9, id: 100
new table is created2: district_dummy_10, id: 101
new table is created2: district_dummy_11, id: 102
new table is created2: district_dummy_12, id: 103
new table is created2: history_dummy_1, id: 104
new table is created2: history_dummy_2, id: 105
new table is created2: history_dummy_3, id: 106
new table is created2: history_dummy_4, id: 107
new table is created2: history_dummy_5, id: 108
new table is created2: history_dummy_6, id: 109
new table is created2: history_dummy_7, id: 110
new table is created2: history_dummy_8, id: 111
new table is created2: history_dummy_9, id: 112
new table is created2: history_dummy_10, id: 113
new table is created2: history_dummy_11, id: 114
new table is created2: history_dummy_12, id: 115
new table is created2: new_order_dummy_1, id: 116
new table is created2: new_order_dummy_2, id: 117
new table is created2: new_order_dummy_3, id: 118
new table is created2: new_order_dummy_4, id: 119
new table is created2: new_order_dummy_5, id: 120
new table is created2: new_order_dummy_6, id: 121
new table is created2: new_order_dummy_7, id: 122
new table is created2: new_order_dummy_8, id: 123
new table is created2: new_order_dummy_9, id: 124
new table is created2: new_order_dummy_10, id: 125
new table is created2: new_order_dummy_11, id: 126
new table is created2: new_order_dummy_12, id: 127
new table is created2: oorder_dummy_1, id: 128
new table is created2: oorder_dummy_2, id: 129
new table is created2: oorder_dummy_3, id: 130
new table is created2: oorder_dummy_4, id: 131
new table is created2: oorder_dummy_5, id: 132
new table is created2: oorder_dummy_6, id: 133
new table is created2: oorder_dummy_7, id: 134
new table is created2: oorder_dummy_8, id: 135
new table is created2: oorder_dummy_9, id: 136
new table is created2: oorder_dummy_10, id: 137
new table is created2: oorder_dummy_11, id: 138
new table is created2: oorder_dummy_12, id: 139
new table is created2: oorder_c_id_idx_dummy_1, id: 140
new table is created2: oorder_c_id_idx_dummy_2, id: 141
new table is created2: oorder_c_id_idx_dummy_3, id: 142
new table is created2: oorder_c_id_idx_dummy_4, id: 143
new table is created2: oorder_c_id_idx_dummy_5, id: 144
new table is created2: oorder_c_id_idx_dummy_6, id: 145
new table is created2: oorder_c_id_idx_dummy_7, id: 146
new table is created2: oorder_c_id_idx_dummy_8, id: 147
new table is created2: oorder_c_id_idx_dummy_9, id: 148
new table is created2: oorder_c_id_idx_dummy_10, id: 149
new table is created2: oorder_c_id_idx_dummy_11, id: 150
new table is created2: oorder_c_id_idx_dummy_12, id: 151
new table is created2: order_line_dummy_1, id: 152
new table is created2: order_line_dummy_2, id: 153
new table is created2: order_line_dummy_3, id: 154
new table is created2: order_line_dummy_4, id: 155
new table is created2: order_line_dummy_5, id: 156
new table is created2: order_line_dummy_6, id: 157
new table is created2: order_line_dummy_7, id: 158
new table is created2: order_line_dummy_8, id: 159
new table is created2: order_line_dummy_9, id: 160
new table is created2: order_line_dummy_10, id: 161
new table is created2: order_line_dummy_11, id: 162
new table is created2: order_line_dummy_12, id: 163
new table is created2: stock_dummy_1, id: 164
new table is created2: stock_dummy_2, id: 165
new table is created2: stock_dummy_3, id: 166
new table is created2: stock_dummy_4, id: 167
new table is created2: stock_dummy_5, id: 168
new table is created2: stock_dummy_6, id: 169
new table is created2: stock_dummy_7, id: 170
new table is created2: stock_dummy_8, id: 171
new table is created2: stock_dummy_9, id: 172
new table is created2: stock_dummy_10, id: 173
new table is created2: stock_dummy_11, id: 174
new table is created2: stock_dummy_12, id: 175
new table is created2: stock_data_dummy_1, id: 176
new table is created2: stock_data_dummy_2, id: 177
new table is created2: stock_data_dummy_3, id: 178
new table is created2: stock_data_dummy_4, id: 179
new table is created2: stock_data_dummy_5, id: 180
new table is created2: stock_data_dummy_6, id: 181
new table is created2: stock_data_dummy_7, id: 182
new table is created2: stock_data_dummy_8, id: 183
new table is created2: stock_data_dummy_9, id: 184
new table is created2: stock_data_dummy_10, id: 185
new table is created2: stock_data_dummy_11, id: 186
new table is created2: stock_data_dummy_12, id: 187
new table is created2: warehouse_dummy_1, id: 188
new table is created2: warehouse_dummy_2, id: 189
new table is created2: warehouse_dummy_3, id: 190
new table is created2: warehouse_dummy_4, id: 191
new table is created2: warehouse_dummy_5, id: 192
new table is created2: warehouse_dummy_6, id: 193
new table is created2: warehouse_dummy_7, id: 194
new table is created2: warehouse_dummy_8, id: 195
new table is created2: warehouse_dummy_9, id: 196
new table is created2: warehouse_dummy_10, id: 197
new table is created2: warehouse_dummy_11, id: 198
new table is created2: warehouse_dummy_12, id: 199
new table is created2: item_dummy_1, id: 200
new table is created2: item_dummy_2, id: 201
new table is created2: item_dummy_3, id: 202
new table is created2: item_dummy_4, id: 203
new table is created2: item_dummy_5, id: 204
new table is created2: item_dummy_6, id: 205
new table is created2: item_dummy_7, id: 206
new table is created2: item_dummy_8, id: 207
new table is created2: item_dummy_9, id: 208
new table is created2: item_dummy_10, id: 209
new table is created2: item_dummy_11, id: 210
new table is created2: item_dummy_12, id: 211
```

### followers
`shard0-p1.log`

```
new table is created: 1
new table is created: 2
new table is created: 3
new table is created: 4
new table is created: 5
new table is created: 6
new table is created: 7
new table is created: 8
new table is created: 9
new table is created: 10
new table is created: 11
new table is created: 12
new table is created: 13
new table is created: 14
new table is created: 15
new table is created: 16
new table is created: 17
new table is created: 18
new table is created: 19
new table is created: 20
new table is created: 21
new table is created: 22
new table is created: 23
new table is created: 24
new table is created: 25
new table is created: 26
new table is created: 27
new table is created: 28
new table is created: 29
new table is created: 30
new table is created: 31
new table is created: 32
new table is created: 33
new table is created: 34
new table is created: 35
new table is created: 36
new table is created: 37
new table is created: 38
new table is created: 39
new table is created: 40
new table is created: 41
new table is created: 42
new table is created: 43
new table is created: 44
new table is created: 45
new table is created: 46
new table is created: 47
new table is created: 48
new table is created: 49
new table is created: 50
new table is created: 51
new table is created: 52
new table is created: 53
new table is created: 54
new table is created: 55
new table is created: 56
new table is created: 57
new table is created: 58
new table is created: 59
new table is created: 60
new table is created: 61
new table is created: 62
new table is created: 63
new table is created: 64
new table is created: 65
new table is created: 66
new table is created: 67
```

### learner
`shard0-learner.log`

Same as followers, but create dummy tables ready for failure recovery, but not used if no failures: `modeMonitor(db, benchConfig.getNthreads(), r)`.

```
new table is created: 1
new table is created: 2
new table is created: 3
new table is created: 4
new table is created: 5
new table is created: 6
new table is created: 7
new table is created: 8
new table is created: 9
new table is created: 10
new table is created: 11
new table is created: 12
new table is created: 13
new table is created: 14
new table is created: 15
new table is created: 16
new table is created: 17
new table is created: 18
new table is created: 19
new table is created: 20
new table is created: 21
new table is created: 22
new table is created: 23
new table is created: 24
new table is created: 25
new table is created: 26
new table is created: 27
new table is created: 28
new table is created: 29
new table is created: 30
new table is created: 31
new table is created: 32
new table is created: 33
new table is created: 34
new table is created: 35
new table is created: 36
new table is created: 37
new table is created: 38
new table is created: 39
new table is created: 40
new table is created: 41
new table is created: 42
new table is created: 43
new table is created: 44
new table is created: 45
new table is created: 46
new table is created: 47
new table is created: 48
new table is created: 49
new table is created: 50
new table is created: 51
new table is created: 52
new table is created: 53
new table is created: 54
new table is created: 55
new table is created: 56
new table is created: 57
new table is created: 58
new table is created: 59
new table is created: 60
new table is created: 61
new table is created: 62
new table is created: 63
new table is created: 64
new table is created: 65
new table is created: 66
new table is created: 67
new table is created2: customer_dummy_1, id: 68
new table is created2: customer_dummy_2, id: 69
new table is created2: customer_dummy_3, id: 70
new table is created2: customer_dummy_4, id: 71
new table is created2: customer_dummy_5, id: 72
new table is created2: customer_dummy_6, id: 73
new table is created2: customer_dummy_7, id: 74
new table is created2: customer_dummy_8, id: 75
new table is created2: customer_dummy_9, id: 76
new table is created2: customer_dummy_10, id: 77
new table is created2: customer_dummy_11, id: 78
new table is created2: customer_dummy_12, id: 79
new table is created2: customer_name_idx_dummy_1, id: 80
new table is created2: customer_name_idx_dummy_2, id: 81
new table is created2: customer_name_idx_dummy_3, id: 82
new table is created2: customer_name_idx_dummy_4, id: 83
new table is created2: customer_name_idx_dummy_5, id: 84
new table is created2: customer_name_idx_dummy_6, id: 85
new table is created2: customer_name_idx_dummy_7, id: 86
new table is created2: customer_name_idx_dummy_8, id: 87
new table is created2: customer_name_idx_dummy_9, id: 88
new table is created2: customer_name_idx_dummy_10, id: 89
new table is created2: customer_name_idx_dummy_11, id: 90
new table is created2: customer_name_idx_dummy_12, id: 91
new table is created2: district_dummy_1, id: 92
new table is created2: district_dummy_2, id: 93
new table is created2: district_dummy_3, id: 94
new table is created2: district_dummy_4, id: 95
new table is created2: district_dummy_5, id: 96
new table is created2: district_dummy_6, id: 97
new table is created2: district_dummy_7, id: 98
new table is created2: district_dummy_8, id: 99
new table is created2: district_dummy_9, id: 100
new table is created2: district_dummy_10, id: 101
new table is created2: district_dummy_11, id: 102
new table is created2: district_dummy_12, id: 103
new table is created2: history_dummy_1, id: 104
new table is created2: history_dummy_2, id: 105
new table is created2: history_dummy_3, id: 106
new table is created2: history_dummy_4, id: 107
new table is created2: history_dummy_5, id: 108
new table is created2: history_dummy_6, id: 109
new table is created2: history_dummy_7, id: 110
new table is created2: history_dummy_8, id: 111
new table is created2: history_dummy_9, id: 112
new table is created2: history_dummy_10, id: 113
new table is created2: history_dummy_11, id: 114
new table is created2: history_dummy_12, id: 115
new table is created2: new_order_dummy_1, id: 116
new table is created2: new_order_dummy_2, id: 117
new table is created2: new_order_dummy_3, id: 118
new table is created2: new_order_dummy_4, id: 119
new table is created2: new_order_dummy_5, id: 120
new table is created2: new_order_dummy_6, id: 121
new table is created2: new_order_dummy_7, id: 122
new table is created2: new_order_dummy_8, id: 123
new table is created2: new_order_dummy_9, id: 124
new table is created2: new_order_dummy_10, id: 125
new table is created2: new_order_dummy_11, id: 126
new table is created2: new_order_dummy_12, id: 127
new table is created2: oorder_dummy_1, id: 128
new table is created2: oorder_dummy_2, id: 129
new table is created2: oorder_dummy_3, id: 130
new table is created2: oorder_dummy_4, id: 131
new table is created2: oorder_dummy_5, id: 132
new table is created2: oorder_dummy_6, id: 133
new table is created2: oorder_dummy_7, id: 134
new table is created2: oorder_dummy_8, id: 135
new table is created2: oorder_dummy_9, id: 136
new table is created2: oorder_dummy_10, id: 137
new table is created2: oorder_dummy_11, id: 138
new table is created2: oorder_dummy_12, id: 139
new table is created2: oorder_c_id_idx_dummy_1, id: 140
new table is created2: oorder_c_id_idx_dummy_2, id: 141
new table is created2: oorder_c_id_idx_dummy_3, id: 142
new table is created2: oorder_c_id_idx_dummy_4, id: 143
new table is created2: oorder_c_id_idx_dummy_5, id: 144
new table is created2: oorder_c_id_idx_dummy_6, id: 145
new table is created2: oorder_c_id_idx_dummy_7, id: 146
new table is created2: oorder_c_id_idx_dummy_8, id: 147
new table is created2: oorder_c_id_idx_dummy_9, id: 148
new table is created2: oorder_c_id_idx_dummy_10, id: 149
new table is created2: oorder_c_id_idx_dummy_11, id: 150
new table is created2: oorder_c_id_idx_dummy_12, id: 151
new table is created2: order_line_dummy_1, id: 152
new table is created2: order_line_dummy_2, id: 153
new table is created2: order_line_dummy_3, id: 154
new table is created2: order_line_dummy_4, id: 155
new table is created2: order_line_dummy_5, id: 156
new table is created2: order_line_dummy_6, id: 157
new table is created2: order_line_dummy_7, id: 158
new table is created2: order_line_dummy_8, id: 159
new table is created2: order_line_dummy_9, id: 160
new table is created2: order_line_dummy_10, id: 161
new table is created2: order_line_dummy_11, id: 162
new table is created2: order_line_dummy_12, id: 163
new table is created2: stock_dummy_1, id: 164
new table is created2: stock_dummy_2, id: 165
new table is created2: stock_dummy_3, id: 166
new table is created2: stock_dummy_4, id: 167
new table is created2: stock_dummy_5, id: 168
new table is created2: stock_dummy_6, id: 169
new table is created2: stock_dummy_7, id: 170
new table is created2: stock_dummy_8, id: 171
new table is created2: stock_dummy_9, id: 172
new table is created2: stock_dummy_10, id: 173
new table is created2: stock_dummy_11, id: 174
new table is created2: stock_dummy_12, id: 175
new table is created2: stock_data_dummy_1, id: 176
new table is created2: stock_data_dummy_2, id: 177
new table is created2: stock_data_dummy_3, id: 178
new table is created2: stock_data_dummy_4, id: 179
new table is created2: stock_data_dummy_5, id: 180
new table is created2: stock_data_dummy_6, id: 181
new table is created2: stock_data_dummy_7, id: 182
new table is created2: stock_data_dummy_8, id: 183
new table is created2: stock_data_dummy_9, id: 184
new table is created2: stock_data_dummy_10, id: 185
new table is created2: stock_data_dummy_11, id: 186
new table is created2: stock_data_dummy_12, id: 187
new table is created2: warehouse_dummy_1, id: 188
new table is created2: warehouse_dummy_2, id: 189
new table is created2: warehouse_dummy_3, id: 190
new table is created2: warehouse_dummy_4, id: 191
new table is created2: warehouse_dummy_5, id: 192
new table is created2: warehouse_dummy_6, id: 193
new table is created2: warehouse_dummy_7, id: 194
new table is created2: warehouse_dummy_8, id: 195
new table is created2: warehouse_dummy_9, id: 196
new table is created2: warehouse_dummy_10, id: 197
new table is created2: warehouse_dummy_11, id: 198
new table is created2: warehouse_dummy_12, id: 199
new table is created2: item_dummy_1, id: 200
new table is created2: item_dummy_2, id: 201
new table is created2: item_dummy_3, id: 202
new table is created2: item_dummy_4, id: 203
new table is created2: item_dummy_5, id: 204
new table is created2: item_dummy_6, id: 205
new table is created2: item_dummy_7, id: 206
new table is created2: item_dummy_8, id: 207
new table is created2: item_dummy_9, id: 208
new table is created2: item_dummy_10, id: 209
new table is created2: item_dummy_11, id: 210
new table is created2: item_dummy_12, id: 211
```