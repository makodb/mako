use super::{codec::*, *};
use std::collections::BTreeSet;

#[derive(Clone, Debug)]
pub struct OrderItem {
    pub item: u32,
    pub supplier: u32,
    pub quantity: u8,
}
#[derive(Clone, Debug)]
pub struct NewOrderRequest {
    pub warehouse: u32,
    pub district: u32,
    pub customer: u32,
    pub date: u32,
    pub items: Vec<OrderItem>,
}
#[derive(Clone, Debug)]
pub struct NewOrderResult {
    pub order: u32,
    pub total: f32,
    pub remote: bool,
}
#[derive(Clone, Debug)]
pub enum CustomerSelector {
    Id(u32),
    LastName(Vec<u8>),
}
#[derive(Clone, Debug)]
pub struct PaymentRequest {
    pub warehouse: u32,
    pub district: u32,
    pub customer_warehouse: u32,
    pub customer_district: u32,
    pub customer: CustomerSelector,
    pub amount: f32,
    pub date: u32,
}
#[derive(Clone, Debug)]
pub struct PaymentResult {
    pub customer: u32,
    pub balance: f32,
    pub bad_credit: bool,
}
#[derive(Clone, Debug)]
pub struct DeliveredOrder {
    pub district: u32,
    pub order: u32,
    pub customer: u32,
    pub amount: f32,
}
#[derive(Clone, Debug)]
pub struct OrderStatus {
    pub customer: u32,
    pub order: u32,
    pub balance: f32,
    pub lines: Vec<OrderLine>,
}

impl Context<'_> {
    pub fn new_order(
        &self,
        tx: &mut Transaction<'_, '_>,
        request: &NewOrderRequest,
    ) -> Result<NewOrderResult> {
        let w = self.district(request.warehouse, request.district)?;
        if request.customer == 0
            || request.customer > self.scale.customers
            || !(5..=15).contains(&request.items.len())
        {
            return Err(invalid("invalid NewOrder request"));
        }
        for item in &request.items {
            self.local_warehouse(item.supplier)?;
            if !(1..=10).contains(&item.quantity) {
                return Err(invalid("invalid order quantity"));
            }
        }
        let warehouse: Warehouse = self.read(tx, WAREHOUSE, request.warehouse, &key(&[w]))?;
        let dkey = key(&[w, request.district]);
        let mut district: District = self.read(tx, DISTRICT, request.warehouse, &dkey)?;
        let customer: Customer = self.read(
            tx,
            CUSTOMER,
            request.warehouse,
            &key(&[w, request.district, request.customer]),
        )?;
        let order = u32::try_from(district.next_order)
            .ok()
            .filter(|id| *id > 0)
            .ok_or_else(|| invalid("invalid next order ID"))?;
        district.next_order = district
            .next_order
            .checked_add(1)
            .ok_or_else(|| invalid("order IDs exhausted"))?;
        self.put(tx, DISTRICT, request.warehouse, &dkey, &district)?;
        let okey = key(&[w, request.district, order]);
        let remote = request
            .items
            .iter()
            .any(|item| item.supplier != request.warehouse);
        self.insert(
            tx,
            NEW_ORDER,
            request.warehouse,
            &okey,
            &NewOrder::default(),
        )?;
        self.insert(
            tx,
            ORDER,
            request.warehouse,
            &okey,
            &Order {
                customer: request.customer as i32,
                carrier: 0,
                lines: request.items.len() as u8,
                all_local: !remote,
                date: request.date,
            },
        )?;
        self.insert(
            tx,
            ORDER_INDEX,
            request.warehouse,
            &key(&[w, request.district, request.customer, order]),
            &OrderIndex::default(),
        )?;
        let mut total = 0f32;
        for (index, selected) in request.items.iter().enumerate() {
            // A missing item rejects the whole body, including headers already
            // staged and any remote stock locks acquired by earlier lines.
            let item: Item = self.read(tx, ITEM, 0, &key(&[selected.item]))?;
            let supply = self.local_warehouse(selected.supplier)?;
            let skey = key(&[supply, selected.item]);
            let mut stock: Stock = self.read(tx, STOCK, selected.supplier, &skey)?;
            let quantity = i16::from(selected.quantity);
            stock.quantity = if stock.quantity - quantity >= 10 {
                stock.quantity - quantity
            } else {
                stock.quantity + 91 - quantity
            };
            stock.ytd += f32::from(selected.quantity);
            if selected.supplier != request.warehouse {
                stock.remote = stock
                    .remote
                    .checked_add(1)
                    .ok_or_else(|| invalid("stock remote counter exhausted"))?;
            }
            // The existing Mako workload leaves s_order_cnt unchanged.
            self.put(tx, STOCK, selected.supplier, &skey, &stock)?;
            let amount = item.price * f32::from(selected.quantity);
            total += amount;
            self.insert(
                tx,
                ORDER_LINE,
                request.warehouse,
                &key(&[w, request.district, order, index as u32 + 1]),
                &OrderLine {
                    item: selected.item as i32,
                    delivery: 0,
                    amount,
                    supply: supply as i32,
                    quantity: selected.quantity,
                },
            )?;
        }
        Ok(NewOrderResult {
            order,
            total: total * (1.0 - customer.discount) * (1.0 + warehouse.tax + district.tax),
            remote,
        })
    }

    fn customer(
        &self,
        tx: &mut Transaction<'_, '_>,
        warehouse: u32,
        district: u32,
        selector: &CustomerSelector,
    ) -> Result<u32> {
        let w = self.district(warehouse, district)?;
        match selector {
            CustomerSelector::Id(id) if *id > 0 && *id <= self.scale.customers => Ok(*id),
            CustomerSelector::Id(_) => Err(invalid("customer ID outside workload")),
            CustomerSelector::LastName(last) => {
                let lower = name_key(w, district, last, &[0; 16])?;
                let upper = name_key(w, district, last, &[255; 16])?;
                let rows = self.rows(tx, CUSTOMER_NAME, warehouse, lower..upper, false, 3001)?;
                if rows.is_empty() {
                    return Err(invalid("customer last name is absent"));
                }
                // Lower median for an even-sized, first-name-sorted set.
                let value = CustomerName::decode(&rows[(rows.len() - 1) / 2].value)?;
                u32::try_from(value.customer)
                    .ok()
                    .filter(|id| *id > 0 && *id <= self.scale.customers)
                    .ok_or_else(|| invalid("customer-name index contains invalid ID"))
            }
        }
    }

    pub fn payment(
        &self,
        tx: &mut Transaction<'_, '_>,
        request: &PaymentRequest,
    ) -> Result<PaymentResult> {
        let w = self.district(request.warehouse, request.district)?;
        let cw = self.district(request.customer_warehouse, request.customer_district)?;
        if !request.amount.is_finite() || !(1.0..=5000.0).contains(&request.amount) {
            return Err(invalid("invalid payment amount"));
        }
        let mut warehouse: Warehouse = self.read(tx, WAREHOUSE, request.warehouse, &key(&[w]))?;
        warehouse.ytd += request.amount;
        self.put(tx, WAREHOUSE, request.warehouse, &key(&[w]), &warehouse)?;
        let dkey = key(&[w, request.district]);
        let mut district: District = self.read(tx, DISTRICT, request.warehouse, &dkey)?;
        district.ytd += request.amount;
        self.put(tx, DISTRICT, request.warehouse, &dkey, &district)?;
        let customer_id = self.customer(
            tx,
            request.customer_warehouse,
            request.customer_district,
            &request.customer,
        )?;
        let ckey = key(&[cw, request.customer_district, customer_id]);
        let mut customer: Customer = self.read(tx, CUSTOMER, request.customer_warehouse, &ckey)?;
        customer.balance -= request.amount;
        customer.ytd_payment += request.amount;
        customer.payment_count = customer
            .payment_count
            .checked_add(1)
            .ok_or_else(|| invalid("customer payment counter exhausted"))?;
        self.put(tx, CUSTOMER, request.customer_warehouse, &ckey, &customer)?;
        let bad_credit = &customer.credit.0 == b"BC";
        if bad_credit {
            let key = key(&[cw, request.customer_district + 100, customer_id]);
            let old: CustomerData = self.read(tx, CUSTOMER, request.customer_warehouse, &key)?;
            let mut value = format!(
                "{} {} {} {} {} {} | ",
                customer_id,
                request.customer_district + 100,
                cw,
                request.district,
                w,
                request.amount as i32
            )
            .into_bytes();
            value.extend_from_slice(&old.data.0);
            value.truncate(300);
            self.put(
                tx,
                CUSTOMER,
                request.customer_warehouse,
                &key,
                &CustomerData {
                    data: Inline(value),
                },
            )?;
        }
        let mut data = warehouse.name.0.clone();
        data.extend_from_slice(b"    ");
        data.extend_from_slice(&district.name.0);
        data.truncate(24);
        // Preserve the historical payment constructor order used by the
        // C++ driver and the retained fused kernel's caller-provided key.
        let hkey = key(&[
            request.customer_district,
            cw,
            customer_id,
            request.district,
            w,
            request.date,
        ]);
        self.insert(
            tx,
            HISTORY,
            request.warehouse,
            &hkey,
            &History {
                amount: request.amount,
                data: Inline(data),
            },
        )?;
        Ok(PaymentResult {
            customer: customer_id,
            balance: customer.balance,
            bad_credit,
        })
    }

    pub fn delivery(
        &self,
        tx: &mut Transaction<'_, '_>,
        warehouse: u32,
        carrier: u32,
        date: u32,
    ) -> Result<Vec<DeliveredOrder>> {
        let w = self.local_warehouse(warehouse)?;
        if !(1..=10).contains(&carrier) {
            return Err(invalid("invalid delivery carrier"));
        }
        let mut delivered = Vec::new();
        for district in 1..=self.scale.districts {
            let oldest = self.rows(
                tx,
                NEW_ORDER,
                warehouse,
                key(&[w, district, 0])..key(&[w, district + 1, 0]),
                false,
                1,
            )?;
            let Some(oldest) = oldest.first() else {
                continue;
            };
            let order_id = key_field(&oldest.key, 2)?;
            let okey = key(&[w, district, order_id]);
            let mut order: Order = self.read(tx, ORDER, warehouse, &okey)?;
            let lines = self.rows(
                tx,
                ORDER_LINE,
                warehouse,
                key(&[w, district, order_id, 0])..key(&[w, district, order_id + 1, 0]),
                false,
                16,
            )?;
            if lines.len() != usize::from(order.lines) || !(5..=15).contains(&lines.len()) {
                return Err(invalid("delivery order-line count mismatch"));
            }
            let mut amount = 0f32;
            for row in lines {
                let mut line = OrderLine::decode(&row.value)?;
                amount += line.amount;
                line.delivery = date;
                self.put(tx, ORDER_LINE, warehouse, &row.key, &line)?;
            }
            tx.mutate(
                &self.target(NEW_ORDER, warehouse, &okey)?,
                Mutation::Remove,
                &okey,
                &[],
            )?;
            order.carrier = carrier as i32;
            self.put(tx, ORDER, warehouse, &okey, &order)?;
            let customer = u32::try_from(order.customer)
                .ok()
                .filter(|id| *id > 0 && *id <= self.scale.customers)
                .ok_or_else(|| invalid("order customer ID"))?;
            // Delivery balances live in the existing customer d+200 subspace.
            let bkey = key(&[w, district + 200, customer]);
            let mut balance: Balance = self.read(tx, CUSTOMER, warehouse, &bkey)?;
            balance.balance += amount;
            self.put(tx, CUSTOMER, warehouse, &bkey, &balance)?;
            delivered.push(DeliveredOrder {
                district,
                order: order_id,
                customer,
                amount,
            });
        }
        Ok(delivered)
    }

    pub fn order_status(
        &self,
        tx: &mut Transaction<'_, '_>,
        warehouse: u32,
        district: u32,
        selector: &CustomerSelector,
    ) -> Result<OrderStatus> {
        let w = self.district(warehouse, district)?;
        let customer_id = self.customer(tx, warehouse, district, selector)?;
        let customer: Customer =
            self.read(tx, CUSTOMER, warehouse, &key(&[w, district, customer_id]))?;
        let orders = self.rows(
            tx,
            ORDER_INDEX,
            warehouse,
            key(&[w, district, customer_id, 0])..key(&[w, district, customer_id + 1, 0]),
            true,
            1,
        )?;
        let order = orders
            .first()
            .ok_or_else(|| invalid("customer has no order"))?;
        let order_id = key_field(&order.key, 3)?;
        let rows = self.rows(
            tx,
            ORDER_LINE,
            warehouse,
            key(&[w, district, order_id, 0])..key(&[w, district, order_id + 1, 0]),
            false,
            16,
        )?;
        if !(5..=15).contains(&rows.len()) {
            return Err(invalid("order status line count"));
        }
        Ok(OrderStatus {
            customer: customer_id,
            order: order_id,
            balance: customer.balance,
            lines: rows
                .iter()
                .map(|row| OrderLine::decode(&row.value))
                .collect::<Result<_>>()?,
        })
    }

    pub fn stock_level(
        &self,
        tx: &mut Transaction<'_, '_>,
        warehouse: u32,
        district: u32,
        threshold: u32,
    ) -> Result<usize> {
        let w = self.district(warehouse, district)?;
        if !(10..=20).contains(&threshold) {
            return Err(invalid("stock-level threshold"));
        }
        let district_row: District = self.read(tx, DISTRICT, warehouse, &key(&[w, district]))?;
        let next = u32::try_from(district_row.next_order).map_err(|_| invalid("next order ID"))?;
        let rows = self.rows(
            tx,
            ORDER_LINE,
            warehouse,
            key(&[w, district, next.saturating_sub(20), 0])..key(&[w, district, next, 0]),
            false,
            301,
        )?;
        if rows.len() > 300 {
            return Err(invalid("stock-level order-line bound"));
        }
        let items: BTreeSet<_> = rows
            .iter()
            .map(|row| OrderLine::decode(&row.value).map(|line| line.item))
            .collect::<Result<_>>()?;
        let mut count = 0;
        for item in items {
            let item = u32::try_from(item)
                .ok()
                .filter(|item| *item > 0 && *item <= self.scale.items)
                .ok_or_else(|| invalid("stock-level item ID"))?;
            let stock: Stock = self.read(tx, STOCK, warehouse, &key(&[w, item]))?;
            if i32::from(stock.quantity) < threshold as i32 {
                count += 1;
            }
        }
        Ok(count)
    }
}
