// Canonical Rust source for the srpc.rand module.
// Compiled directly by rustc and translated by rusty-cpp crate mode.
pub fn randgen_rand_max() -> f64 {
    i32::MAX as f64
}

#[cfg_attr(any(), cpp_abi(
    param(s, std_string_bytes),
    returns(std_string_bytes)
))]
pub fn randgen_zero_pad(s: Vec<u8>, length: i32) -> Vec<u8> {
    let mut ret = s;
    while (ret.len() as i32) < length {
        ret.insert(0usize, 48u8);
    }
    while (ret.len() as i32) > length {
        ret.remove(0usize);
    }
    ret
}

#[cfg_attr(any(), cpp_abi_alias(std_vector))]
pub type RandWeightVec = Vec<f64>;

pub struct RandomGenerator {}

impl RandomGenerator {
    pub fn rand(min: i32, max: i32) -> i32 {
        assert!(max >= min);
        let r = randgen_rand_raw();
        let width = max.wrapping_sub(min).wrapping_add(1i32);
        assert!(width != 0i32);
        (r % width).wrapping_add(min)
    }

    pub fn rand_double(min: f64, max: f64) -> f64 {
        if max == min {
            return min;
        }
        assert!(max > min);
        let r = randgen_rand_raw();
        ((r as f64) / (randgen_rand_max() / (max - min))) + min
    }

    #[cfg_attr(any(), cpp_abi(returns(std_string_bytes)))]
    pub fn int2str_n(i: i32, length: i32) -> Vec<u8> {
        let negative = i < 0i32;
        let mut magnitude: u32 = i.unsigned_abs();
        let mut reversed = Vec::<u8>::new();
        loop {
            reversed.push(((magnitude % 10u32) as u8).wrapping_add(48u8));
            magnitude /= 10u32;
            if magnitude == 0u32 {
                break;
            }
        }
        let mut s = Vec::<u8>::with_capacity(
            reversed.len() + if negative { 1usize } else { 0usize },
        );
        if negative {
            s.push(45u8);
        }
        let mut position = reversed.len();
        while position > 0usize {
            position -= 1usize;
            s.push(reversed[position]);
        }
        randgen_zero_pad(s, length)
    }

    pub fn percentage_true(p: i32) -> bool {
        RandomGenerator::rand(0, 99) < p
    }

    pub fn nu_rand(a: i32, x: i32, y: i32) -> i32 {
        let r1 = RandomGenerator::rand(0, a);
        let r2 = RandomGenerator::rand(x, y);
        let width = y.wrapping_sub(x).wrapping_add(1i32);
        assert!(width != 0i32);
        ((r1 | r2).wrapping_add(randgen_nu_constant_now()) % width)
            .wrapping_add(x)
    }

    #[cfg_attr(any(), cpp_abi(param(
        weight_vector,
        const_ref(RandWeightVec)
    )))]
    pub fn weighted_select(weight_vector: &[f64]) -> u32 {
        let mut sum: f64 = 0.0;
        let mut i: usize = 0usize;
        while i < weight_vector.len() {
            sum += weight_vector[i];
            i += 1usize;
        }
        let r = RandomGenerator::rand_double(0.0, sum);
        let mut stage_sum: f64 = 0.0;
        let mut k: usize = 0usize;
        while k < weight_vector.len() {
            stage_sum += weight_vector[k];
            if r <= stage_sum {
                return k as u32;
            }
            k += 1usize;
        }
        (k as u32).wrapping_sub(1u32)
    }

    pub fn destroy() {
        randgen_destroy()
    }
}

#[allow(unsafe_code)]
unsafe extern "C" {
    fn srpc_rand_raw() -> i32;
    fn srpc_rand_destroy();
}

// @unsafe - thin shim over the C kernel.
#[allow(unsafe_code)]
pub fn randgen_rand_raw() -> i32 {
    unsafe { srpc_rand_raw() }
}

// @safe - the historical nu_rand constant was always zero.
pub fn randgen_nu_constant_now() -> i32 {
    0
}

// @unsafe - thin shim over the C kernel (pthread teardown lives there).
#[allow(unsafe_code)]
pub fn randgen_destroy() {
    unsafe { srpc_rand_destroy(); }
}
